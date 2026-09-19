// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <key.h>
#include <test/util/setup_common.h>
#include <test/util/source_root.h>
#include <uint256.h>
#include <util/strencodings.h>
#include <wallet/digidollarwallet.h>
#include <wallet/test/util.h>
#include <wallet/test/wallet_test_fixture.h>
#include <wallet/wallet.h>

#include <string>

BOOST_FIXTURE_TEST_SUITE(digidollar_wallet_hd_tests, wallet::WalletTestingSetup)

BOOST_AUTO_TEST_CASE(wave1_qt_and_rpc_mint_paths_use_hd_owner_derivation)
{
    const std::string qt_source = ReadRepositoryFile("src/qt/walletmodel.cpp");
    const std::string rpc_source = ReadRepositoryFile("src/rpc/digidollar.cpp");

    BOOST_CHECK_NE(qt_source.find("GetHDKeyForDigiDollar(\"dd-owner\")"), std::string::npos);
    BOOST_CHECK_NE(rpc_source.find("GetHDKeyForDigiDollar(\"dd-owner\")"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(wave1_qt_persists_owner_key_before_broadcast)
{
    // The mint in the wallet window has to write the owner key and the
    // position record before it sends the transaction. RecordPendingMint
    // writes both and reports which one failed; what this check guards is
    // that it is still called before the send. The behaviour itself is tested
    // in the Qt suite, which can run a real mint
    // (qt/test/digidollarmintrecordtests.cpp).
    const std::string qt_source = ReadRepositoryFile("src/qt/walletmodel.cpp");

    const size_t mint_pos = qt_source.find("WalletModel::mintDigiDollar");
    BOOST_REQUIRE_NE(mint_pos, std::string::npos);
    const size_t next_method_pos = qt_source.find("\nWalletModel::", mint_pos + 1);
    const std::string mint_source = next_method_pos == std::string::npos
        ? qt_source.substr(mint_pos)
        : qt_source.substr(mint_pos, next_method_pos - mint_pos);

    const size_t commit_pos = mint_source.find("CommitTransaction(txRef");
    const size_t store_pos = mint_source.find("RecordPendingMint(");

    BOOST_REQUIRE_NE(commit_pos, std::string::npos);
    BOOST_REQUIRE_NE(store_pos, std::string::npos);
    BOOST_CHECK_MESSAGE(store_pos < commit_pos,
        "Qt mint path saves the DD owner key and position after broadcast; RecordPendingMint offset="
        << store_pos << " commitTransaction offset=" << commit_pos);
}

BOOST_AUTO_TEST_CASE(wave1_non_hd_wallet_fails_clearly_for_dd_owner_key)
{
    wallet::CWallet non_hd_wallet(m_node.chain.get(), "wave1-non-hd", wallet::CreateMockableWalletDatabase());
    non_hd_wallet.LoadWallet();
    non_hd_wallet.SetWalletFlag(wallet::WALLET_FLAG_DISABLE_PRIVATE_KEYS);

    CKey owner_key;
    {
        LOCK(non_hd_wallet.cs_wallet);
        owner_key = non_hd_wallet.GetHDKeyForDigiDollar("dd-owner");
    }

    BOOST_CHECK_MESSAGE(!owner_key.IsValid(),
        "non-HD/private-key-disabled wallet produced a random valid DD owner key instead of failing clearly");
}

BOOST_AUTO_TEST_CASE(wave1_legacy_wallet_dd_owner_derivation_is_unreachable)
{
    const std::string wallet_source = ReadRepositoryFile("src/wallet/wallet.cpp");
    const size_t helper_pos = wallet_source.find("CKey CWallet::GetHDKeyForDigiDollar");
    BOOST_REQUIRE_NE(helper_pos, std::string::npos);
    const size_t helper_end = wallet_source.find("\n}", helper_pos);
    BOOST_REQUIRE_NE(helper_end, std::string::npos);
    const std::string helper_source = wallet_source.substr(helper_pos, helper_end - helper_pos);

    const size_t descriptor_guard = helper_source.find("!IsWalletFlagSet(WALLET_FLAG_DESCRIPTORS)");
    const size_t legacy_fallback = helper_source.find("LegacyScriptPubKeyMan");
    BOOST_REQUIRE_NE(descriptor_guard, std::string::npos);
    BOOST_REQUIRE_NE(legacy_fallback, std::string::npos);
    BOOST_CHECK_MESSAGE(descriptor_guard < legacy_fallback,
        "DigiDollar owner/address derivation can still reach a legacy wallet key fallback");
}

BOOST_AUTO_TEST_CASE(wave1_persisted_qt_minted_owner_key_recovers_after_wallet_reload)
{
    m_wallet.EnsureDDWallet();
    DigiDollarWallet* dd_wallet = m_wallet.GetDDWallet();
    BOOST_REQUIRE(dd_wallet != nullptr);

    const uint256 position_id = uint256S("00000000000000000000000000000000000000000000000000000000dd000001");
    CKey owner_key;
    owner_key.MakeNewKey(true);
    dd_wallet->StoreOwnerKey(position_id, owner_key);

    auto restored_db = wallet::DuplicateMockDatabase(m_wallet.GetDatabase());
    wallet::CWallet restored_wallet(m_node.chain.get(), "wave1-restored", std::move(restored_db));
    restored_wallet.LoadWallet();
    restored_wallet.EnsureDDWallet();

    DigiDollarWallet* restored_dd_wallet = restored_wallet.GetDDWallet();
    BOOST_REQUIRE(restored_dd_wallet != nullptr);
    BOOST_CHECK_GE(restored_dd_wallet->LoadDDOwnerKeys(), 1U);

    CKey recovered_key;
    BOOST_REQUIRE(restored_dd_wallet->GetOwnerKey(position_id, recovered_key));
    BOOST_CHECK(recovered_key.GetPubKey() == owner_key.GetPubKey());
}

BOOST_AUTO_TEST_CASE(dd_address_key_is_not_cached_when_the_database_refuses_it)
{
    // getdigidollaraddress may only hand out an address whose key reached the
    // wallet file. A key kept in memory after a refused write looks usable
    // until the next restart, and anything sent to that address would then be
    // unspendable, so the write has to report the failure and leave nothing
    // behind.
    m_wallet.EnsureDDWallet();
    DigiDollarWallet* dd_wallet = m_wallet.GetDDWallet();
    BOOST_REQUIRE(dd_wallet != nullptr);

    CKey address_key;
    address_key.MakeNewKey(true);
    const XOnlyPubKey output_key(address_key.GetPubKey());

    wallet::GetMockableDatabase(m_wallet).m_pass = false;
    BOOST_CHECK(!dd_wallet->StoreAddressKey(output_key, address_key));
    CKey reloaded;
    BOOST_CHECK(!dd_wallet->GetAddressKey(output_key, reloaded));

    wallet::GetMockableDatabase(m_wallet).m_pass = true;
    BOOST_CHECK(dd_wallet->StoreAddressKey(output_key, address_key));
    BOOST_REQUIRE(dd_wallet->GetAddressKey(output_key, reloaded));
    BOOST_CHECK(reloaded.GetPubKey() == address_key.GetPubKey());
}

BOOST_AUTO_TEST_SUITE_END()
