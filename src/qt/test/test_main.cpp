// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2014-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#if defined(HAVE_CONFIG_H)
#include <config/digibyte-config.h>
#endif

#include <interfaces/init.h>
#include <interfaces/node.h>
#include <qt/digibyte.h>
#include <qt/guiconstants.h>
#include <qt/guiutil.h>
#include <qt/test/apptests.h>
#include <qt/test/optiontests.h>
#include <qt/test/rpcnestedtests.h>
#include <qt/test/uritests.h>
#include <test/util/setup_common.h>
#include <util/chaintype.h>

#ifdef ENABLE_WALLET
#include <qt/test/addressbooktests.h>
#include <qt/test/ddtransactiontabletests.h>
#include <qt/test/wallettests.h>
#include <qt/test/digidollarwidgettests.h>
#include <qt/test/digidollarwave19widgettests.h>
#include <qt/test/digidollarmintrecordtests.h>
#include <qt/test/ddtransactionrecordtests.h>
#endif // ENABLE_WALLET

#include <QApplication>
#include <QDebug>
#include <QObject>
#include <QStringList>
#include <QTest>

#include <functional>

#if defined(QT_STATICPLUGIN)
#include <QtPlugin>
#if defined(QT_QPA_PLATFORM_MINIMAL)
Q_IMPORT_PLUGIN(QMinimalIntegrationPlugin);
#endif
#if defined(QT_QPA_PLATFORM_XCB)
Q_IMPORT_PLUGIN(QXcbIntegrationPlugin);
#elif defined(QT_QPA_PLATFORM_WINDOWS)
Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin);
#elif defined(QT_QPA_PLATFORM_COCOA)
Q_IMPORT_PLUGIN(QCocoaIntegrationPlugin);
#elif defined(QT_QPA_PLATFORM_ANDROID)
Q_IMPORT_PLUGIN(QAndroidPlatformIntegrationPlugin)
#endif
#endif

const std::function<void(const std::string&)> G_TEST_LOG_FUN{};

const std::function<std::vector<const char*>()> G_TEST_COMMAND_LINE_ARGUMENTS{};

// This is all you need to run all the tests
int main(int argc, char* argv[])
{
    // Initialize persistent globals with the testing setup state for sanity.
    // E.g. -datadir in gArgs is set to a temp directory dummy value (instead
    // of defaulting to the default datadir), or globalChainParams is set to
    // regtest params.
    //
    // All tests must use their own testing setup (if needed).
    fs::create_directories([] {
        BasicTestingSetup dummy{ChainType::REGTEST};
        return gArgs.GetDataDirNet() / "blocks";
    }());

    std::unique_ptr<interfaces::Init> init = interfaces::MakeGuiInit(argc, argv);
    gArgs.ForceSetArg("-listen", "0");
    gArgs.ForceSetArg("-listenonion", "0");
    gArgs.ForceSetArg("-discover", "0");
    gArgs.ForceSetArg("-dnsseed", "0");
    gArgs.ForceSetArg("-fixedseeds", "0");
    gArgs.ForceSetArg("-upnp", "0");
    gArgs.ForceSetArg("-natpmp", "0");

    std::string error;
    if (!gArgs.ReadConfigFiles(error, true)) QWARN(error.c_str());

    // Prefer the "minimal" platform for the test instead of the normal default
    // platform ("xcb", "windows", or "cocoa") so tests can't unintentionally
    // interfere with any background GUIs and don't require extra resources.
    #if defined(WIN32)
        if (getenv("QT_QPA_PLATFORM") == nullptr) _putenv_s("QT_QPA_PLATFORM", "minimal");
    #else
        setenv("QT_QPA_PLATFORM", "minimal", 0 /* overwrite */);
    #endif

    // Don't remove this, it's needed to access
    // QApplication:: and QCoreApplication:: in the tests
    DigiByteApplication app;
    app.setOrganizationName(QAPP_ORG_NAME);
    app.setOrganizationDomain(QAPP_ORG_DOMAIN);
    app.setApplicationName("DigiByte-Qt-test");
    app.createNode(*init);

    int num_test_failures{0};
    const QStringList requested_suites = GUIUtil::SplitSkipEmptyParts(
        qEnvironmentVariable("DIGIBYTE_QT_TEST_SUITE"), QLatin1Char(','));
    QStringList test_arguments{QStringLiteral("test_digibyte-qt")};
    const QString requested_function = qEnvironmentVariable("DIGIBYTE_QT_TEST_FUNCTION").trimmed();
    if (!requested_function.isEmpty()) test_arguments.append(requested_function);
    const QString test_output = qEnvironmentVariable("DIGIBYTE_QT_TEST_OUTPUT").trimmed();
    if (!test_output.isEmpty()) {
        test_arguments.append(QStringLiteral("-o"));
        test_arguments.append(test_output + QStringLiteral(",txt"));
    }
    const auto run_test = [&](QObject& test) {
        const QString suite_name = QString::fromLatin1(test.metaObject()->className());
        if (!requested_suites.isEmpty() && !requested_suites.contains(suite_name)) {
            qInfo("Skipping Qt test suite %s (DIGIBYTE_QT_TEST_SUITE filter)", qPrintable(suite_name));
            return;
        }
        num_test_failures += QTest::qExec(&test, test_arguments);
    };

    app.node().context()->args = &gArgs;
    
    AppTests app_tests(app);
    run_test(app_tests);

    app.node().context()->args = &gArgs;

    OptionTests options_tests(app.node());
    run_test(options_tests);

    URITests test1;
    run_test(test1);

    RPCNestedTests test3(app.node());
    run_test(test3);

#ifdef ENABLE_WALLET
    WalletTests test5(app.node());
    run_test(test5);

    AddressBookTests test6(app.node());
    run_test(test6);

    DigiDollarWidgetTests test7(app.node());
    run_test(test7);

    // Wave 19 Agent B: separate translation unit for the Wave 19 Qt pins
    // (DD-FA-FUNC-030, DD-FA-TEST-027/028/029) — kept out of
    // digidollarwidgettests.cpp to avoid concurrent edits in the audit.
    DigiDollarWave19WidgetTests test8(app.node());
    run_test(test8);

    // Separate translation unit for the mint save-before-send order, kept out
    // of digidollarwidgettests.cpp so that work on the two files does not
    // collide.
    DigiDollarMintRecordTests test9(app.node());
    run_test(test9);

    DDTransactionRecordTests test10(app.node());
    run_test(test10);

    DDTransactionTableTests test11(app.node());
    run_test(test11);
#endif

    if (num_test_failures) {
        qWarning("\nFailed tests: %d\n", num_test_failures);
    } else {
        qDebug("\nAll tests passed.\n");
    }
    return num_test_failures;
}
