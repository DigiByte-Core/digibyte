// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <digidollar/digidollar.h>
#include <digidollar/validation.h>
#include <interfaces/chain.h>
#include <oracle/bundle_manager.h>
#include <oracle/mock_oracle.h>
#include <rpc/digidollar.h>
#include <rpc/server.h>
#include <test/util/setup_common.h>
#include <validation.h>
#include <wallet/context.h>
#include <wallet/digidollarwallet.h>
#include <wallet/digidollarmintcapability.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace {

struct CandidateWalletSetup : TestChain100Setup {
    wallet::WalletContext context;
    std::shared_ptr<wallet::CWallet> test_wallet;

    CandidateWalletSetup()
        : TestChain100Setup(ChainType::REGTEST,
                           {"-digidollaractivationheight=100", "-ddthawdayheight=120"})
    {
        MockOracleManager::GetInstance().Reset();
        OracleBundleManager::GetInstance().Clear();
        mineBlocks(20);
        auto& mock = MockOracleManager::GetInstance();
        mock.SetEnabled(true);
        mock.SetMockPrice(500000);
        OracleBundleManager::GetInstance().SetEnabled(true);
        BOOST_REQUIRE(OracleBundleManager::GetInstance().UpdateBundle(mock.CreateMockMuSig2Bundle(121)));

        test_wallet = std::make_shared<wallet::CWallet>(m_node.chain.get(), "candidate-lock-wallet",
                                                       wallet::CreateMockableWalletDatabase());
        test_wallet->LoadWallet();
        test_wallet->EnsureDDWallet();
        uint256 tip_hash;
        {
            LOCK(cs_main);
            auto& chainman = *m_node.chainman;
            const auto* tip = chainman.ActiveChain().Tip();
            BOOST_REQUIRE_EQUAL(tip->nHeight, 120);
            tip_hash = tip->GetBlockHash();
            CAmount quote{0};
            int health{-1};
            DigiDollar::ChainstateHealth state;
            std::string error;
            BOOST_REQUIRE_MESSAGE(DigiDollar::GetNextBlockOracleQuote(tip, Params().GetConsensus(),
                chainman.m_blockman, quote, error), error);
            BOOST_REQUIRE_MESSAGE(DigiDollar::GetChainstateHealthForNextBlock(tip, Params().GetConsensus(),
                chainman.m_blockman, chainman.ActiveChainstate().CoinsTip(), quote, health, state, error), error);
            BOOST_REQUIRE(state.history_checked);
            BOOST_REQUIRE_EQUAL(health, 30000);
        }
        WITH_LOCK(test_wallet->cs_wallet, test_wallet->SetLastBlockProcessed(120, tip_hash));
        context.args = m_node.args;
        context.chain = m_node.chain.get();
        wallet::AddWallet(context, test_wallet);
    }

    ~CandidateWalletSetup()
    {
        wallet::RemoveWallet(context, test_wallet, std::nullopt);
        MockOracleManager::GetInstance().Reset();
        OracleBundleManager::GetInstance().Clear();
    }

    JSONRPCRequest Request(const std::string& method)
    {
        JSONRPCRequest request;
        request.context = &context;
        request.strMethod = method;
        request.params = UniValue(UniValue::VARR);
        return request;
    }

    void CheckChainAvailableWhileConstructionWaits(const std::function<void()>& construct)
    {
        using namespace std::chrono_literals;
        std::atomic<bool> finished{false};
        bool observed_wallet_wait{false};
        bool chain_available{false};
        std::exception_ptr failure;
        std::thread worker;
        {
            // Pause construction after it has acquired the ordinary wallet lock.
            // An independent observer never blocks on either lock, so regressions
            // fail an assertion instead of hanging the test process.
            LOCK(test_wallet->GetDDWallet()->cs_dd_wallet);
            worker = std::thread([&] {
                try { construct(); } catch (...) { failure = std::current_exception(); }
                finished = true;
            });
            std::thread observer([&] {
                const auto deadline = std::chrono::steady_clock::now() + 5s;
                std::optional<std::chrono::steady_clock::time_point> waiting_since;
                while (!finished && std::chrono::steady_clock::now() < deadline) {
                    bool wallet_available;
                    {
                        TRY_LOCK(test_wallet->cs_wallet, wallet_probe);
                        wallet_available = bool(wallet_probe);
                    }
                    const auto now = std::chrono::steady_clock::now();
                    if (wallet_available) {
                        waiting_since.reset();
                    } else if (!waiting_since) {
                        waiting_since = now;
                    } else if (now - *waiting_since >= 20ms) {
                        observed_wallet_wait = true;
                        TRY_LOCK(cs_main, chain_probe);
                        chain_available = bool(chain_probe);
                        break;
                    }
                    std::this_thread::sleep_for(1ms);
                }
            });
            observer.join();
        }
        worker.join();
        BOOST_REQUIRE_MESSAGE(!failure, "Unexpected exception from wallet construction");
        BOOST_REQUIRE_MESSAGE(observed_wallet_wait, "Construction did not reach the expected wallet wait");
        BOOST_CHECK_MESSAGE(chain_available, "Wallet construction retained cs_main while waiting for the DD wallet lock");
    }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(digidollar_wallet_candidate_lock_tests, CandidateWalletSetup)

BOOST_AUTO_TEST_CASE(activated_mint_releases_chain_before_wallet_selection)
{
    {
        LOCK(test_wallet->cs_wallet);
        test_wallet->m_keypool_size = 2;
        test_wallet->SetWalletFlag(wallet::WALLET_FLAG_DESCRIPTORS);
        test_wallet->SetupDescriptorScriptPubKeyMans();
    }
    BOOST_REQUIRE(wallet::GetDigiDollarMintWalletError(*test_wallet).empty());
    auto request = Request("mintdigidollar");
    request.params.push_back(int64_t{10000});
    request.params.push_back(0);
    std::string error;
    CheckChainAvailableWhileConstructionWaits([&] {
        try { mintdigidollar().HandleRequest(request); }
        catch (const UniValue& result) { error = result.find_value("message").get_str(); }
    });
    BOOST_CHECK_EQUAL(error, "No available UTXOs for collateral");
}

BOOST_AUTO_TEST_CASE(activated_default_redemption_releases_chain_before_wallet_selection)
{
    bool redeemed{true};
    CTransactionRef transaction;
    CheckChainAvailableWhileConstructionWaits([&] {
        redeemed = test_wallet->GetDDWallet()->RedeemDigiDollar(uint256::ONE, 10000, transaction);
    });
    BOOST_CHECK(!redeemed);
    BOOST_CHECK(!transaction);
}

BOOST_AUTO_TEST_CASE(activated_redemption_rpcs_use_wallet_chain_context)
{
    for (const std::string method : {"redeemdigidollar", "getredemptioninfo"}) {
        auto request = Request(method);
        request.params.push_back(uint256::ONE.GetHex());
        if (method == "redeemdigidollar") request.params.push_back(int64_t{10000});
        std::string error;
        try {
            if (method == "redeemdigidollar") redeemdigidollar().HandleRequest(request);
            else getredemptioninfo().HandleRequest(request);
        } catch (const UniValue& result) {
            error = result.find_value("message").get_str();
        }
        BOOST_CHECK_MESSAGE(error.find("not found") != std::string::npos &&
                            error.find("Position") != std::string::npos, error);
    }
}

BOOST_AUTO_TEST_SUITE_END()
