// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_QT_TEST_DIGIDOLLARTESTUTIL_H
#define DIGIBYTE_QT_TEST_DIGIDOLLARTESTUTIL_H

#include <qt/clientmodel.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/walletmodel.h>

#include <memory>
#include <string>

struct TestChain100Setup;
namespace interfaces { class Node; }
namespace wallet { class CWallet; }

namespace DigiDollarTest {
std::shared_ptr<wallet::CWallet> SetupDescriptorsWallet(
    interfaces::Node& node, TestChain100Setup& test, const std::string& wallet_name = "");

struct DigiDollarMiniGUI {
    OptionsModel optionsModel;
    std::unique_ptr<ClientModel> clientModel;
    std::unique_ptr<WalletModel> walletModel;
    std::unique_ptr<const PlatformStyle> platformStyle;

    explicit DigiDollarMiniGUI(interfaces::Node& node);
    void initModelForWallet(interfaces::Node& node, const std::shared_ptr<wallet::CWallet>& wallet);
};
} // namespace DigiDollarTest

#endif // DIGIBYTE_QT_TEST_DIGIDOLLARTESTUTIL_H
