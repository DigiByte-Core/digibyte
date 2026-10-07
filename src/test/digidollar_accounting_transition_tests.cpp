// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <coins.h>
#include <digidollar/health.h>
#include <digidollar/scripts.h>
#include <digidollar/validation.h>
#include <key.h>
#include <script/script.h>
#include <test/util/setup_common.h>
#include <txdb.h>

#include <boost/test/unit_test.hpp>

#include <limits>
#include <map>
#include <utility>
#include <vector>

namespace {

CScript FreshTaproot()
{
    CKey key;
    key.MakeNewKey(true);
    const auto pubkey = key.GetPubKey();
    return CScript{} << OP_1 << std::vector<unsigned char>(pubkey.begin() + 1, pubkey.end());
}

struct AccountingTx {
    CTransactionRef tx;
    uint32_t height;
    std::vector<Coin> inputs;
};

struct InputCoin {
    COutPoint outpoint;
    Coin coin;
};

InputCoin Output(const AccountingTx& source, uint32_t index)
{
    return {COutPoint{source.tx->GetHash(), index}, Coin{source.tx->vout.at(index), static_cast<int>(source.height), false}};
}

struct AccountingSetup : BasicTestingSetup {
    Consensus::Params params{Params().GetConsensus()};
    std::map<uint256, std::pair<uint32_t, CTransactionRef>> creating;
    DigiDollar::CanonicalTxLookup lookup;

    AccountingSetup()
    {
        params.DigiDollarHeight = 10;
        lookup = [this](const uint256& hash, uint32_t height, CTransactionRef& tx) {
            const auto found = creating.find(hash);
            if (found == creating.end() || found->second.first != height) return false;
            tx = found->second.second;
            return true;
        };
    }

    AccountingTx Store(const CMutableTransaction& tx, uint32_t height, std::vector<Coin> inputs)
    {
        auto ref = MakeTransactionRef(tx);
        creating.emplace(ref->GetHash(), std::make_pair(height, ref));
        return {ref, height, std::move(inputs)};
    }

    AccountingTx Mint(CAmount principal, CAmount collateral, uint32_t height = 20)
    {
        CMutableTransaction tx;
        tx.nVersion = MakeDigiDollarVersion(DD_TX_MINT);
        tx.vin.emplace_back(COutPoint{uint256S("09"), static_cast<uint32_t>(creating.size())});
        const auto vault_script = FreshTaproot();
        tx.vout.emplace_back(5, CScript{} << OP_TRUE);
        tx.vout.emplace_back(collateral, vault_script);
        tx.vout.emplace_back(0, FreshTaproot());
        tx.vout.emplace_back(0, CScript{} << OP_RETURN << std::vector<unsigned char>{'D', 'D'}
            << CScriptNum(1) << CScriptNum(principal) << CScriptNum(500) << CScriptNum(0)
            << std::vector<unsigned char>(vault_script.begin() + 2, vault_script.end()));
        return Store(tx, height, {Coin{CTxOut{collateral + COIN, CScript{} << OP_TRUE}, 1, false}});
    }

    AccountingTx Redeem(const AccountingTx& mint, const std::vector<InputCoin>& tokens,
                        CAmount change, uint32_t height = 600)
    {
        CMutableTransaction tx;
        tx.nVersion = MakeDigiDollarVersion(DD_TX_REDEEM);
        tx.nLockTime = 500;
        tx.vin.emplace_back(Output(mint, 1).outpoint, CScript{}, 0xfffffffe);
        std::vector<Coin> inputs{Output(mint, 1).coin};
        for (const auto& token : tokens) {
            tx.vin.emplace_back(token.outpoint, CScript{}, 0xfffffffe);
            inputs.push_back(token.coin);
        }
        tx.vout.emplace_back(mint.tx->vout[1].nValue, FreshTaproot());
        if (change > 0) {
            tx.vout.emplace_back(0, FreshTaproot());
            tx.vout.emplace_back(0, CScript{} << OP_RETURN << std::vector<unsigned char>{'D', 'D'}
                << CScriptNum(3) << CScriptNum(change));
        }
        return Store(tx, height, std::move(inputs));
    }

    AccountingTx Transfer(const std::vector<InputCoin>& tokens, const std::vector<CAmount>& amounts,
                          uint32_t height)
    {
        CMutableTransaction tx;
        tx.nVersion = MakeDigiDollarVersion(DD_TX_TRANSFER);
        std::vector<Coin> inputs;
        for (const auto& token : tokens) {
            tx.vin.emplace_back(token.outpoint);
            inputs.push_back(token.coin);
        }
        CScript metadata = CScript{} << OP_RETURN << std::vector<unsigned char>{'D', 'D'} << CScriptNum(2);
        for (CAmount amount : amounts) {
            tx.vout.emplace_back(0, FreshTaproot());
            metadata << CScriptNum(amount);
        }
        tx.vout.emplace_back(0, metadata);
        return Store(tx, height, std::move(inputs));
    }

    AccountingTx Burn(const InputCoin& token, uint32_t height)
    {
        CMutableTransaction tx;
        tx.vin.emplace_back(token.outpoint);
        tx.vout.emplace_back(0, CScript{} << OP_RETURN);
        return Store(tx, height, {token.coin});
    }

    DigiDollar::ChainstateHealth EmptyHealth() const
    {
        DigiDollar::ChainstateHealth state;
        state.genesis_hash = params.hashGenesisBlock;
        state.best_block = uint256S("100");
        state.history_checked = true;
        return state;
    }

    CAmount Apply(const AccountingTx& tx, DigiDollar::ChainstateHealth& health, CAmount& supply)
    {
        CAmount delta{0};
        std::string error;
        BOOST_REQUIRE_MESSAGE(DigiDollar::CalculateCirculatingSupplyChange(
            *tx.tx, tx.inputs, tx.height, params, lookup, delta, error), error);
        BOOST_REQUIRE_MESSAGE(DigiDollar::UpdateChainstateHealth(
            *tx.tx, tx.inputs, params, lookup, health, false, error), error);
        supply += delta;
        return delta;
    }
};

class RegistryOverride {
    CScript m_script;
    DigiDollar::ScriptMetadata m_restore;

public:
    RegistryOverride(const CScript& script, DigiDollar::ScriptMetadata replacement,
                     DigiDollar::ScriptMetadata canonical)
        : m_script{script}, m_restore{canonical}
    {
        DigiDollar::GetScriptMetadata(script, m_restore);
        DigiDollar::RegisterScriptMetadata(script, replacement.type, replacement.ddAmount, replacement.lockHeight);
    }
    ~RegistryOverride()
    {
        // There is no public erase API. Restore a prior entry when present,
        // otherwise retain the fixture's correct metadata for its unique script.
        DigiDollar::RegisterScriptMetadata(m_script, m_restore.type, m_restore.ddAmount, m_restore.lockHeight);
    }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(digidollar_accounting_transition_tests, AccountingSetup)

BOOST_AUTO_TEST_CASE(ordinary_vault_redemption_tracks_principal_and_actual_token_change)
{
    const auto first = Mint(100000, 1000 * COIN);
    const auto second = Mint(200000, 2000 * COIN);
    auto initial = EmptyHealth();
    CAmount initial_supply{0};
    BOOST_CHECK_EQUAL(Apply(first, initial, initial_supply), 100000);
    BOOST_CHECK_EQUAL(Apply(second, initial, initial_supply), 200000);
    for (bool extra_tokens : {false, true}) {
        auto health = initial;
        CAmount supply = initial_supply;
        std::vector<InputCoin> tokens{Output(first, 2)};
        if (extra_tokens) tokens.push_back(Output(second, 2));
        const auto redeem = Redeem(first, tokens, extra_tokens ? 200000 : 0);
        BOOST_CHECK_EQUAL(Apply(redeem, health, supply), -100000);
        BOOST_CHECK_EQUAL(health.open_vault_principal, 200000);
        BOOST_CHECK_EQUAL(health.collateral, 2000 * COIN);
        BOOST_CHECK_EQUAL(health.active_vaults, 1U);
        BOOST_CHECK_EQUAL(supply, 200000);
        BOOST_CHECK(DigiDollar::CalculateChainstateHealth(health, 1000000) == 100);
        std::string error;
        BOOST_REQUIRE_MESSAGE(DigiDollar::UpdateChainstateHealth(
            *redeem.tx, redeem.inputs, params, lookup, health, true, error), error);
        BOOST_CHECK(health == initial);
    }
}

BOOST_AUTO_TEST_CASE(emergency_excess_burn_with_change_does_not_reduce_other_vault_principal)
{
    const auto first = Mint(100000, 1000 * COIN);
    const auto second = Mint(200000, 2000 * COIN);
    auto health = EmptyHealth();
    CAmount supply{0};
    Apply(first, health, supply);
    Apply(second, health, supply);
    const auto initial = health;
    BOOST_CHECK(DigiDollar::CalculateChainstateHealth(health, 800000) == 80);
    // Closing the first vault burns 125000 at 80% health. Of 300000 token
    // inputs, 175000 return as DD change; the second vault stays fully open.
    const auto redeem = Redeem(first, {Output(first, 2), Output(second, 2)}, 175000);
    BOOST_CHECK_EQUAL(Apply(redeem, health, supply), -125000);
    BOOST_CHECK_EQUAL(health.open_vault_principal, 200000);
    BOOST_CHECK_EQUAL(health.collateral, 2000 * COIN);
    BOOST_CHECK_EQUAL(health.active_vaults, 1U);
    BOOST_CHECK_EQUAL(supply, 175000);
    BOOST_CHECK(DigiDollar::CalculateChainstateHealth(health, 800000) == 80);
    std::string error;
    BOOST_REQUIRE_MESSAGE(DigiDollar::UpdateChainstateHealth(
        *redeem.tx, redeem.inputs, params, lookup, health, true, error), error);
    BOOST_CHECK(health == initial);
}

BOOST_AUTO_TEST_CASE(token_burn_changes_supply_without_closing_a_vault)
{
    const auto mint = Mint(100000, 1000 * COIN);
    auto health = EmptyHealth();
    CAmount supply{0};
    Apply(mint, health, supply);
    const auto before = health;
    const auto burn = Burn(Output(mint, 2), 30);
    BOOST_CHECK_EQUAL(Apply(burn, health, supply), -100000);
    BOOST_CHECK_EQUAL(supply, 0);
    BOOST_CHECK(health == before);
    BOOST_CHECK(DigiDollar::CalculateChainstateHealth(health, 1000000) == 100);
    std::string error;
    BOOST_REQUIRE_MESSAGE(DigiDollar::UpdateChainstateHealth(
        *burn.tx, burn.inputs, params, lookup, health, true, error), error);
    BOOST_CHECK(health == before);
}

BOOST_AUTO_TEST_CASE(transaction_order_updates_have_exact_reverse_order_undo)
{
    const auto first = Mint(100000, 1000 * COIN, 20);
    const auto second = Mint(200000, 2000 * COIN, 20);
    const auto transfer = Transfer({Output(first, 2), Output(second, 2)}, {140000, 160000}, 21);
    const auto redeem = Redeem(first, {Output(transfer, 0), Output(transfer, 1)}, 175000, 600);
    const auto burn = Burn(Output(redeem, 1), 601);
    const std::vector<AccountingTx> transactions{first, second, transfer, redeem, burn};
    const std::vector<CAmount> expected_delta{100000, 200000, 0, -125000, -175000};
    std::vector<DigiDollar::ChainstateHealth> previous_health;
    std::vector<CAmount> previous_supply;
    auto health = EmptyHealth();
    CAmount supply{0};
    for (size_t i = 0; i < transactions.size(); ++i) {
        previous_health.push_back(health);
        previous_supply.push_back(supply);
        BOOST_CHECK_EQUAL(Apply(transactions[i], health, supply), expected_delta[i]);
        BOOST_CHECK(health.IsValid());
    }
    BOOST_CHECK_EQUAL(health.open_vault_principal, 200000);
    BOOST_CHECK_EQUAL(supply, 0);
    for (size_t i = transactions.size(); i-- > 0;) {
        const auto& tx = transactions[i];
        std::string error;
        BOOST_REQUIRE_MESSAGE(DigiDollar::UpdateChainstateHealth(
            *tx.tx, tx.inputs, params, lookup, health, true, error), error);
        supply -= expected_delta[i];
        BOOST_CHECK(health == previous_health[i]);
        BOOST_CHECK_EQUAL(supply, previous_supply[i]);
    }
    BOOST_CHECK(health == EmptyHealth());
}

BOOST_AUTO_TEST_CASE(canonical_vault_and_token_amounts_ignore_process_registry)
{
    const auto mint = Mint(625013, 500 * COIN);
    const auto source = Output(mint, 1);
    DigiDollar::CanonicalVault vault;
    std::string error;
    BOOST_REQUIRE(DigiDollar::LookupCanonicalVault(source.outpoint, source.coin, params, lookup, vault, error) ==
                  DigiDollar::VaultLookupResult::VAULT);
    BOOST_CHECK_EQUAL(vault.principal, 625013);
    const auto collateral_type = DigiDollar::ScriptType::COLLATERAL_LOCK;
    const auto token_type = DigiDollar::ScriptType::DD_TOKEN_OUTPUT;
    RegistryOverride vault_metadata{source.coin.out.scriptPubKey, {collateral_type, 9, 1}, {collateral_type, 625013, 500}};
    RegistryOverride token_metadata{mint.tx->vout[2].scriptPubKey, {collateral_type, 7, 1}, {token_type, 625013, 0}};
    RegistryOverride amount_metadata{mint.tx->vout[3].scriptPubKey, {token_type, 11, 1}, {token_type, 625013, 500}};
    BOOST_REQUIRE(DigiDollar::LookupCanonicalVault(source.outpoint, source.coin, params, lookup, vault, error) ==
                  DigiDollar::VaultLookupResult::VAULT);
    BOOST_CHECK(vault.outpoint == source.outpoint);
    BOOST_CHECK_EQUAL(vault.principal, 625013);
    BOOST_CHECK_EQUAL(vault.collateral, 500 * COIN);
    auto health = EmptyHealth();
    CAmount supply{0};
    BOOST_CHECK_EQUAL(Apply(mint, health, supply), 625013);
    BOOST_CHECK_EQUAL(health.open_vault_principal, 625013);
    BOOST_CHECK_EQUAL(supply, 625013);
    const auto token = Output(mint, 2);
    BOOST_CHECK(DigiDollar::LookupCanonicalVault(token.outpoint, token.coin, params, lookup, vault, error) ==
                DigiDollar::VaultLookupResult::NOT_VAULT);
    CMutableTransaction ordinary;
    ordinary.vin.emplace_back(COutPoint{uint256S("99"), 0});
    ordinary.vout.push_back(source.coin.out);
    const auto regular = Store(ordinary, 21, {});
    const auto regular_output = Output(regular, 0);
    BOOST_CHECK(DigiDollar::LookupCanonicalVault(regular_output.outpoint, regular_output.coin, params, lookup, vault, error) ==
                DigiDollar::VaultLookupResult::NOT_VAULT);
}

BOOST_AUTO_TEST_CASE(canonical_lookup_distinguishes_ineligible_coins_from_missing_provenance)
{
    const auto mint = Mint(100000, 1000 * COIN);
    const auto source = Output(mint, 1);
    const auto other = Mint(200000, 2000 * COIN);
    auto not_ready = [&](const COutPoint& outpoint, const Coin& coin, const DigiDollar::CanonicalTxLookup& reader) {
        DigiDollar::CanonicalVault vault{source.outpoint, 1, 1};
        std::string error;
        BOOST_CHECK(DigiDollar::LookupCanonicalVault(outpoint, coin, params, reader, vault, error) ==
                    DigiDollar::VaultLookupResult::NOT_READY);
        BOOST_CHECK(!error.empty());
        BOOST_CHECK(vault.outpoint.IsNull());
        BOOST_CHECK_EQUAL(vault.principal, 0);
        BOOST_CHECK_EQUAL(vault.collateral, 0);
    };
    not_ready(source.outpoint, source.coin, {});
    not_ready(COutPoint{uint256S("dead"), 1}, source.coin, lookup);
    not_ready(COutPoint{mint.tx->GetHash(), 0}, source.coin, lookup);
    not_ready(COutPoint{mint.tx->GetHash(), static_cast<uint32_t>(mint.tx->vout.size())}, source.coin, lookup);
    Coin wrong_height = source.coin;
    ++wrong_height.nHeight;
    not_ready(source.outpoint, wrong_height, lookup);
    Coin wrong_value = source.coin;
    ++wrong_value.out.nValue;
    not_ready(source.outpoint, wrong_value, lookup);
    not_ready(source.outpoint, source.coin, [&](const uint256&, uint32_t, CTransactionRef& tx) {
        tx = other.tx;
        return true;
    });
    not_ready(source.outpoint, source.coin, [](const uint256&, uint32_t, CTransactionRef& tx) {
        tx.reset();
        return true;
    });
    const std::vector<Coin> ineligible{
        Coin{}, Coin{source.coin.out, 9, false}, Coin{source.coin.out, 20, true},
        Output(mint, 0).coin, Output(mint, 2).coin};
    for (const auto& coin : ineligible) {
        DigiDollar::CanonicalVault vault;
        std::string error;
        BOOST_CHECK(DigiDollar::LookupCanonicalVault(source.outpoint, coin, params, {}, vault, error) ==
                    DigiDollar::VaultLookupResult::NOT_VAULT);
    }
}

BOOST_AUTO_TEST_CASE(failed_helper_updates_preserve_the_complete_previous_state)
{
    const auto first = Mint(100000, 1000 * COIN);
    const auto second = Mint(200000, 2000 * COIN);
    auto health = EmptyHealth();
    CAmount supply{0};
    Apply(first, health, supply);
    Apply(second, health, supply);
    const auto before = health;
    const auto redeem = Redeem(first, {Output(first, 2)}, 0);
    CMutableTransaction unavailable{*redeem.tx};
    unavailable.vin.emplace_back(COutPoint{uint256S("feed"), 0});
    auto inputs = redeem.inputs;
    inputs.emplace_back(CTxOut{COIN, FreshTaproot()}, 20, false);
    std::string error;
    BOOST_CHECK(!DigiDollar::UpdateChainstateHealth(CTransaction{unavailable}, inputs, params, lookup, health, false, error));
    BOOST_CHECK(health == before);
    BOOST_CHECK(!error.empty());
    BOOST_CHECK(!DigiDollar::UpdateChainstateHealth(*redeem.tx, {}, params, lookup, health, false, error));
    BOOST_CHECK(health == before);

    auto near_limit = before;
    near_limit.open_vault_principal = std::numeric_limits<CAmount>::max() - 50000;
    const auto limit_before = near_limit;
    BOOST_CHECK(!DigiDollar::UpdateChainstateHealth(*first.tx, first.inputs, params, lookup, near_limit, false, error));
    BOOST_CHECK(near_limit == limit_before);
    auto empty = EmptyHealth();
    BOOST_CHECK(!DigiDollar::UpdateChainstateHealth(*first.tx, first.inputs, params, lookup, empty, true, error));
    BOOST_CHECK(empty == EmptyHealth());

    CAmount delta{123};
    BOOST_CHECK(!DigiDollar::CalculateCirculatingSupplyChange(
        *redeem.tx, redeem.inputs, redeem.height, params, {}, delta, error));
    BOOST_CHECK_EQUAL(delta, 123);
    BOOST_CHECK(!DigiDollar::CalculateCirculatingSupplyChange(
        *redeem.tx, {}, redeem.height, params, lookup, delta, error));
    BOOST_CHECK_EQUAL(delta, 123);
}


// A mint that is already in the chain but whose amount cannot be read is not the
// same thing as a block the node has lost. Every node reading the same block gets
// the same answer, so such an output is simply not a collateral vault. Treating it
// as missing data would stop every upgraded node at the Thaw Day height with no
// way out, because the "restore the block" advice is about a block it already has.
BOOST_AUTO_TEST_CASE(unreadable_legacy_mint_is_not_a_vault)
{
    // A mint whose DigiDollar OP_RETURN carries no amount at all.
    CMutableTransaction no_amount;
    no_amount.nVersion = MakeDigiDollarVersion(DD_TX_MINT);
    no_amount.vin.emplace_back(COutPoint{uint256S("11"), 0});
    no_amount.vout.emplace_back(500 * COIN, FreshTaproot());
    no_amount.vout.emplace_back(0, CScript{} << OP_RETURN << std::vector<unsigned char>{'D', 'D'}
        << CScriptNum(1));
    const auto unreadable = Store(no_amount, 20, {});
    const auto unreadable_vault = Output(unreadable, 0);

    // A mint with two positive canonical Taproot outputs, so no single output can
    // be named as the collateral.
    CMutableTransaction two_vaults;
    two_vaults.nVersion = MakeDigiDollarVersion(DD_TX_MINT);
    two_vaults.vin.emplace_back(COutPoint{uint256S("12"), 0});
    two_vaults.vout.emplace_back(300 * COIN, FreshTaproot());
    two_vaults.vout.emplace_back(200 * COIN, FreshTaproot());
    two_vaults.vout.emplace_back(0, CScript{} << OP_RETURN << std::vector<unsigned char>{'D', 'D'}
        << CScriptNum(1) << CScriptNum(100000) << CScriptNum(500) << CScriptNum(0));
    const auto ambiguous = Store(two_vaults, 20, {});
    const auto ambiguous_vault = Output(ambiguous, 0);

    for (const auto& source : {unreadable_vault, ambiguous_vault}) {
        DigiDollar::CanonicalVault vault;
        std::string error;
        BOOST_CHECK(DigiDollar::LookupCanonicalVault(source.outpoint, source.coin, params, lookup, vault, error) ==
                    DigiDollar::VaultLookupResult::NOT_VAULT);
        BOOST_CHECK(vault.outpoint.IsNull());
        BOOST_CHECK_EQUAL(vault.principal, 0);
        BOOST_CHECK_EQUAL(vault.collateral, 0);
    }

    // The same coins in a reconstruction: left out of the totals, and the
    // reconstruction still finishes instead of reporting that data is missing.
    CCoinsViewDB db{{.path = m_args.GetDataDirNet() / "dd-unreadable-mint", .cache_bytes = 1 << 20, .memory_only = true}, {}};
    CCoinsViewCache cache{&db};
    cache.SetBestBlock(uint256S("21"));
    for (const auto& source : {unreadable_vault, ambiguous_vault}) {
        cache.AddCoin(source.outpoint, Coin{source.coin}, false);
    }
    const auto good = Mint(100000, 1000 * COIN);
    const auto good_vault = Output(good, 1);
    cache.AddCoin(good_vault.outpoint, Coin{good_vault.coin}, false);
    DigiDollar::ChainstateHealth health;
    std::string error;
    BOOST_REQUIRE_MESSAGE(DigiDollar::ReconstructChainstateHealth(cache, params, lookup, health, error), error);
    BOOST_CHECK_EQUAL(health.active_vaults, 1U);
    BOOST_CHECK_EQUAL(health.open_vault_principal, 100000);
    BOOST_CHECK_EQUAL(health.collateral, 1000 * COIN);

    // A creating block the node really cannot read is still a readiness failure.
    DigiDollar::CanonicalVault vault;
    BOOST_CHECK(DigiDollar::LookupCanonicalVault(good_vault.outpoint, good_vault.coin, params, {}, vault, error) ==
                DigiDollar::VaultLookupResult::NOT_READY);
    BOOST_CHECK(!error.empty());
}

BOOST_AUTO_TEST_CASE(spent_vault_is_not_recounted_before_cache_flush)
{
    const CAmount principal = 100000;
    const CAmount collateral = 1000 * COIN;
    const auto original = Mint(principal, collateral);
    CMutableTransaction tx{*original.tx};
    tx.vout.assign(16513, CTxOut{1, CScript{} << OP_TRUE});
    tx.vout[256] = original.tx->vout[1];
    tx.vout[0] = original.tx->vout[2];
    tx.vout[1] = original.tx->vout[3];
    const auto mint = Store(tx, original.height, original.inputs);
    const auto vault = Output(mint, 256);
    DigiDollar::CanonicalVault canonical;
    std::string error;
    BOOST_REQUIRE(DigiDollar::LookupCanonicalVault(vault.outpoint, vault.coin, params, lookup, canonical, error) ==
                  DigiDollar::VaultLookupResult::VAULT);

    CCoinsViewDB db{{.path = m_args.GetDataDirNet() / "dd-spent-vault", .cache_bytes = 1 << 20, .memory_only = true}, {}};
    CCoinsViewCache cache{&db};
    cache.SetBestBlock(uint256S("21"));
    for (uint32_t index : {128, 256, 16512}) {
        const auto output = Output(mint, index);
        cache.AddCoin(output.outpoint, Coin{output.coin}, false);
    }
    BOOST_REQUIRE(cache.Flush());
    DigiDollar::ChainstateHealth health;
    BOOST_REQUIRE_MESSAGE(DigiDollar::ReconstructChainstateHealth(cache, params, lookup, health, error), error);
    BOOST_CHECK_EQUAL(health.active_vaults, 1U);
    BOOST_CHECK_EQUAL(health.open_vault_principal, principal);
    BOOST_CHECK_EQUAL(health.collateral, collateral);

    BOOST_REQUIRE(cache.SpendCoin(vault.outpoint));
    for (bool flush : {false, true}) {
        if (flush) BOOST_REQUIRE(cache.Flush());
        BOOST_REQUIRE_MESSAGE(DigiDollar::ReconstructChainstateHealth(cache, params, lookup, health, error), error);
        BOOST_CHECK_EQUAL(health.active_vaults, 0U);
        BOOST_CHECK_EQUAL(health.open_vault_principal, 0);
        BOOST_CHECK_EQUAL(health.collateral, 0);
    }
}

BOOST_AUTO_TEST_SUITE_END()
