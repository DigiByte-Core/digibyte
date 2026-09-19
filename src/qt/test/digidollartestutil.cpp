// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/digidollartestutil.h>

#include <interfaces/chain.h>
#include <interfaces/node.h>
#include <key_io.h>
#include <test/util/setup_common.h>
#include <validation.h>
#include <wallet/context.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>

#include <QTest>

using wallet::AddWallet;
using wallet::CreateMockableWalletDatabase;
using wallet::RemoveWallet;
using wallet::WALLET_FLAG_DESCRIPTORS;
using wallet::WalletContext;
using wallet::WalletRescanReserver;

namespace DigiDollarTest {
static void SyncUpWallet(const std::shared_ptr<wallet::CWallet>& wallet, interfaces::Node& node)
{
    WalletRescanReserver reserver(*wallet);
    reserver.reserve();
    wallet::CWallet::ScanResult result = wallet->ScanForWalletTransactions(
        Params().GetConsensus().hashGenesisBlock, 0, {}, reserver, true, false);
    QCOMPARE(result.status, wallet::CWallet::ScanResult::SUCCESS);
}

std::shared_ptr<wallet::CWallet> SetupDescriptorsWallet(interfaces::Node& node, TestChain100Setup& test, const std::string& wallet_name)
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
    CTxDestination dest = GetDestinationForKey(test.coinbaseKey.GetPubKey(), wallet->m_default_address_type);
    wallet->SetAddressBook(dest, "", wallet::AddressPurpose::RECEIVE);
    wallet->SetLastBlockProcessed(105, WITH_LOCK(node.context()->chainman->GetMutex(),
        return node.context()->chainman->ActiveChain().Tip()->GetBlockHash()));
    SyncUpWallet(wallet, node);
    wallet->SetBroadcastTransactions(true);
    return wallet;
}

DigiDollarMiniGUI::DigiDollarMiniGUI(interfaces::Node& node) : optionsModel(node)
{
    bilingual_str error;
    QVERIFY(optionsModel.Init(error));
    clientModel = std::make_unique<ClientModel>(node, &optionsModel);
    platformStyle.reset(PlatformStyle::instantiate("other"));
}

void DigiDollarMiniGUI::initModelForWallet(interfaces::Node& node, const std::shared_ptr<wallet::CWallet>& wallet)
{
    WalletContext& context = *node.walletLoader().context();
    AddWallet(context, wallet);
    walletModel = std::make_unique<WalletModel>(
        interfaces::MakeWallet(context, wallet), *clientModel, platformStyle.get());
    RemoveWallet(context, wallet, std::nullopt);
}
} // namespace DigiDollarTest
