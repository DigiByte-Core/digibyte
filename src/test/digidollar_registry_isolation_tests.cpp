// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * Consensus must read DigiDollar amounts from the chain, never from the
 * in-memory script metadata registry.
 *
 * The registry (RegisterScriptMetadata / GetScriptMetadata) is filled in by the
 * wallet and the transaction builder. It is keyed by output script alone, so a
 * DigiDollar owner address that is used again keeps only the amount written
 * last. A mainnet node holding such a wallet rejected the first DigiDollar mint
 * block during a reindex: the registry said the mint's token output was worth
 * $98 while the chain said $100. Block validity must not depend on what a
 * local wallet has done since.
 *
 * Each test below writes wrong values into the registry and checks that
 * validation still answers from chain data.
 */

#include <coins.h>
#include <consensus/validation.h>
#include <consensus/volatility.h>
#include <digidollar/digidollar.h>
#include <digidollar/health.h>
#include <digidollar/scripts.h>
#include <digidollar/validation.h>
#include <kernel/chainparams.h>
#include <key.h>
#include <primitives/transaction.h>
#include <pubkey.h>
#include <script/script.h>
#include <script/standard.h>
#include <test/util/setup_common.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

#include <string>
#include <vector>

namespace {

struct RegistryIsolationSetup : BasicTestingSetup {
    DigiDollar::SystemMetrics previous_metrics{DigiDollar::SystemHealthMonitor::GetCachedMetrics()};

    RegistryIsolationSetup()
    {
        DigiDollar::SystemHealthMonitor::ResetMetrics();
        DigiDollar::Volatility::VolatilityMonitor::ClearHistory();
    }
    ~RegistryIsolationSetup()
    {
        DigiDollar::SystemHealthMonitor::RestoreLegacyMetrics(previous_metrics);
        DigiDollar::Volatility::VolatilityMonitor::ClearHistory();
    }
};

struct BuiltMint {
    CMutableTransaction tx;
    CScript token_script;
    CScript collateral_script;
};

/** Build a mint that passes ValidateMintTransaction in the given context. */
BuiltMint BuildValidMint(const CChainParams& params, const DigiDollar::ValidationContext& ctx,
                         CAmount dd_amount, bool op_return_before_token)
{
    const int64_t lock_blocks = DigiDollar::LockDaysToBlocks(30);
    const int tier = DigiDollar::GetLockTierIndex(lock_blocks, params.GetDigiDollarParams());
    BOOST_REQUIRE_GE(tier, 0);
    const CAmount collateral = DigiDollar::CalculateRequiredCollateral(dd_amount, lock_blocks, ctx);
    BOOST_REQUIRE_GT(collateral, 1);

    CKey owner_key;
    owner_key.MakeNewKey(true);
    const XOnlyPubKey owner(owner_key.GetPubKey());

    DigiDollar::MintParams mint_params;
    mint_params.ddAmount = dd_amount;
    mint_params.lockHeight = ctx.nHeight + lock_blocks;
    mint_params.ownerKey = owner;
    mint_params.internalKey = DigiDollar::GetCollateralNUMSKey();
    mint_params.oracleKeys = DigiDollar::GetOracleKeys(15);

    BuiltMint built;
    // The test writes the registry itself, so the vault script is not registered here.
    built.collateral_script = DigiDollar::CreateCollateralP2TR(mint_params, /*register_metadata=*/false);
    BOOST_REQUIRE(!built.collateral_script.empty());
    built.token_script = DigiDollar::CreateDigiDollarP2TR(owner, dd_amount);
    BOOST_REQUIRE(!built.token_script.empty());

    const CScript op_return = CScript() << OP_RETURN << std::vector<unsigned char>{'D', 'D'}
                                        << CScriptNum(1) << CScriptNum(dd_amount)
                                        << CScriptNum(mint_params.lockHeight) << CScriptNum(tier)
                                        << std::vector<unsigned char>(owner.begin(), owner.end());

    built.tx.nVersion = 0x01000770;
    built.tx.vin.emplace_back(COutPoint(uint256::ONE, 0));
    built.tx.vout.emplace_back(collateral, built.collateral_script);
    if (op_return_before_token) {
        built.tx.vout.emplace_back(0, op_return);
        built.tx.vout.emplace_back(0, built.token_script);
    } else {
        built.tx.vout.emplace_back(0, built.token_script);
        built.tx.vout.emplace_back(0, op_return);
    }
    built.tx.vout.emplace_back(COIN, GetScriptForDestination(WitnessV0KeyHash(owner_key.GetPubKey())));
    return built;
}

void CheckMintAccepted(const CMutableTransaction& mint, const DigiDollar::ValidationContext& ctx,
                       const std::string& what)
{
    TxValidationState state;
    const bool accepted = DigiDollar::ValidateMintTransaction(CTransaction(mint), ctx, state);
    BOOST_CHECK_MESSAGE(accepted, what + ": " + state.ToString());
    BOOST_CHECK_MESSAGE(state.IsValid(), what + ": " + state.ToString());
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(digidollar_registry_isolation_tests, RegistryIsolationSetup)

// The mainnet case from the rc1 reindex. Block 23,869,549 holds a $100 mint.
// The owner address later received $99 and then $98 of change, so the wallet
// left $98 in the registry. $98 is below the mainnet minimum mint.
BOOST_AUTO_TEST_CASE(mainnet_mint_block_ignores_stale_registry_amount)
{
    const auto params = CChainParams::Main();
    const int height = 24'000'000; // DigiDollar active, $100 minimum enforced, before Thaw Day
    const DigiDollar::ValidationContext ctx(height, 30'000, 150, *params);
    BOOST_REQUIRE(!DigiDollar::IsThawDayActive(params->GetConsensus(), height));
    BOOST_REQUIRE(DigiDollar::ValidateMintAmount(10'000, *params, height));
    BOOST_REQUIRE(!DigiDollar::ValidateMintAmount(9'800, *params, height));

    const BuiltMint mint = BuildValidMint(*params, ctx, 10'000, /*op_return_before_token=*/false);
    CheckMintAccepted(mint.tx, ctx, "valid mint before the wallet touched the registry");

    DigiDollar::RegisterScriptMetadata(mint.token_script, DigiDollar::ScriptType::DD_TOKEN_OUTPUT, 9'800, 0);
    CheckMintAccepted(mint.tx, ctx, "same mint after the wallet left $98 in the registry");
}

// Regtest allows one-cent mints, so a stale smaller amount passes the minimum
// there. A stale amount above the maximum, or a stale amount seen before the
// OP_RETURN, still made rc1 reject the mint.
BOOST_AUTO_TEST_CASE(regtest_mint_ignores_stale_registry_amount_in_any_output_order)
{
    const auto params = CChainParams::RegTest({});
    const DigiDollar::ValidationContext ctx(1'000, 500'000, 150, *params);
    const CAmount too_big = params->GetDigiDollarParams().maxMintAmount + 50'000;

    for (const bool op_return_first : {false, true}) {
        const BuiltMint mint = BuildValidMint(*params, ctx, 10'000, op_return_first);
        CheckMintAccepted(mint.tx, ctx, "valid mint");

        // A later transfer left less at this address.
        DigiDollar::RegisterScriptMetadata(mint.token_script, DigiDollar::ScriptType::DD_TOKEN_OUTPUT, 9'900, 0);
        CheckMintAccepted(mint.tx, ctx, "stale smaller amount in the registry");

        // A later receive left more than one mint may create.
        DigiDollar::RegisterScriptMetadata(mint.token_script, DigiDollar::ScriptType::DD_TOKEN_OUTPUT, too_big, 0);
        CheckMintAccepted(mint.tx, ctx, "stale larger amount in the registry");

        // Wrong vault terms in the registry must not matter either.
        DigiDollar::RegisterScriptMetadata(mint.collateral_script, DigiDollar::ScriptType::COLLATERAL_LOCK, 1, 1);
        CheckMintAccepted(mint.tx, ctx, "wrong vault terms in the registry");
    }
}

// A non-DigiDollar spend is only a vault spend when the creating transaction on
// the chain is a mint. A registry entry alone must not make it one.
BOOST_AUTO_TEST_CASE(vault_spend_check_ignores_registry_claims)
{
    const auto params = CChainParams::RegTest({});
    CKey key;
    key.MakeNewKey(true);
    const XOnlyPubKey xonly(key.GetPubKey());
    const CScript plain_p2tr = CScript() << OP_1 << ToByteVector(xonly);

    // An ordinary transaction created this coin.
    CMutableTransaction creator;
    creator.vin.emplace_back(COutPoint(uint256::ONE, 0));
    creator.vout.emplace_back(COIN, plain_p2tr);
    const CTransactionRef creator_ref = MakeTransactionRef(creator);
    const COutPoint coin_out(creator_ref->GetHash(), 0);

    CCoinsView base;
    CCoinsViewCache coins(&base);
    coins.AddCoin(coin_out, Coin(CTxOut(COIN, plain_p2tr), 700, false), false);

    const DigiDollar::TxLookupFn lookup = [creator_ref](const uint256& txid, uint32_t, CTransactionRef& out) {
        if (txid != creator_ref->GetHash()) return false;
        out = creator_ref;
        return true;
    };
    const DigiDollar::ValidationContext ctx(701, 500'000, 150, *params, &coins, false, lookup);

    CMutableTransaction spend;
    spend.vin.emplace_back(coin_out);
    spend.vout.emplace_back(COIN - 1000, GetScriptForDestination(WitnessV0KeyHash(key.GetPubKey())));
    BOOST_CHECK(!DigiDollar::SpendsDigiDollarCollateralVault(CTransaction(spend), ctx));

    // The registry claims this script is a vault. Consensus must not believe it.
    DigiDollar::RegisterScriptMetadata(plain_p2tr, DigiDollar::ScriptType::COLLATERAL_LOCK, 10'000, 800);
    BOOST_CHECK(!DigiDollar::SpendsDigiDollarCollateralVault(CTransaction(spend), ctx));
    BOOST_CHECK(!DigiDollar::RequiresDigiDollarValidation(CTransaction(spend), ctx));
}

// The plain amount reader answers from the script alone. Only an explicit
// registry read, which consensus never performs, sees the table.
BOOST_AUTO_TEST_CASE(extract_dd_amount_reads_chain_data_by_default)
{
    CKey key;
    key.MakeNewKey(true);
    const XOnlyPubKey owner(key.GetPubKey());
    const CScript token = DigiDollar::CreateDigiDollarP2TR(owner, 12'345); // registers 12,345 cents
    CAmount amount{0};

    // A plain P2TR output carries no amount on chain.
    BOOST_CHECK(!DigiDollar::ExtractDDAmount(token, amount));
    BOOST_CHECK(DigiDollar::ExtractDDAmount(token, amount, /*allow_registry=*/true));
    BOOST_CHECK_EQUAL(amount, 12'345);

    // A mint OP_RETURN carries its amount in the script itself.
    const CScript op_return = CScript() << OP_RETURN << std::vector<unsigned char>{'D', 'D'}
                                        << CScriptNum(1) << CScriptNum(10'000)
                                        << CScriptNum(500) << CScriptNum(1);
    amount = 0;
    BOOST_CHECK(DigiDollar::ExtractDDAmount(op_return, amount));
    BOOST_CHECK_EQUAL(amount, 10'000);
}

BOOST_AUTO_TEST_SUITE_END()
