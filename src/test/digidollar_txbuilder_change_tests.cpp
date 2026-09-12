// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// These tests cover where the DigiDollar transaction builders send leftover DGB.
// Leftover DGB is the change output. If the builder picks an address the wallet
// does not own, the owner cannot spend that money again. The builders must
// either use the address the caller gives them, or stop and return an error.
//
// A mint has one extra rule. A mint may contain only one taproot output that
// holds DGB, and that output is the locked collateral. If the change output is
// also taproot, the whole mint is rejected by every node. So mint change must be
// an ordinary bech32 (P2WPKH) address.

#include <boost/test/unit_test.hpp>

#include <chainparams.h>
#include <consensus/digidollar.h>
#include <digidollar/txbuilder.h>
#include <digidollar/validation.h>
#include <key.h>
#include <key_io.h>
#include <kernel/chainparams.h>
#include <script/standard.h>
#include <test/util/setup_common.h>

#include <string>
#include <vector>

using namespace DigiDollar;

BOOST_FIXTURE_TEST_SUITE(digidollar_txbuilder_change_tests, BasicTestingSetup)

namespace {

CKey NewKey()
{
    CKey key;
    key.MakeNewKey(true);
    return key;
}

// UTXOs that all share one transaction hash, one per output index.
std::vector<COutPoint> MakeOutPoints(size_t count)
{
    std::vector<COutPoint> points;
    uint256 hash;
    hash.SetHex("1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef");
    for (size_t i = 0; i < count; ++i) {
        points.emplace_back(hash, i);
    }
    return points;
}

uint32_t CanonicalTier(int lockDays)
{
    const int tier = GetLockTierIndex(LockDaysToBlocks(lockDays), Params().GetDigiDollarParams());
    BOOST_REQUIRE_GE(tier, 0);
    return static_cast<uint32_t>(tier);
}

bool IsTaprootScript(const CScript& script)
{
    int version = -1;
    std::vector<unsigned char> program;
    return script.IsWitnessProgram(version, program) && version == 1 &&
           program.size() == WITNESS_V1_TAPROOT_SIZE;
}

// An ordinary bech32 change address, the kind a wallet hands out.
CTxDestination Bech32Destination(const CKey& key)
{
    return CTxDestination{WitnessV0KeyHash(key.GetPubKey())};
}

// A taproot address. A mint must refuse this for change.
CTxDestination TaprootDestination(const CKey& key)
{
    return CTxDestination{WitnessV1Taproot(XOnlyPubKey(key.GetPubKey()))};
}

// Every input is worth 10,000 DGB, so these builds always have change to place.
class BigUtxoMintBuilder : public MintTxBuilder
{
public:
    using MintTxBuilder::MintTxBuilder;
    CAmount GetDGBFromUTXO(const COutPoint&) const override { return 10000 * COIN; }
};

class BigUtxoTransferBuilder : public TransferTxBuilder
{
public:
    using TransferTxBuilder::TransferTxBuilder;
    CAmount GetDGBFromUTXO(const COutPoint&) const override { return 10000 * COIN; }
};

class BigUtxoRedeemBuilder : public RedeemTxBuilder
{
public:
    using RedeemTxBuilder::RedeemTxBuilder;
    CAmount GetDGBFromUTXO(const COutPoint&) const override { return 10000 * COIN; }
};

const int TEST_HEIGHT = 1000;
const CAmount TEST_PRICE = 10000; // one cent per DGB, in millionths of a dollar

TxBuilderMintParams BaseMintParams()
{
    TxBuilderMintParams params;
    params.ddAmount = 10000; // $100.00
    params.lockDays = 365;
    params.lockTier = CanonicalTier(params.lockDays);
    params.ownerKey = NewKey();
    params.feeRate = 100000;
    params.utxos = MakeOutPoints(5);
    return params;
}

TxBuilderTransferParams BaseTransferParams(const CKey& spender, const CKey& recipient)
{
    TxBuilderTransferParams params;
    const std::string recipientAddress =
        EncodeDigiDollarAddress(CTxDestination{WitnessV1Taproot(XOnlyPubKey(recipient.GetPubKey()))}, Params());
    params.recipients = {{recipientAddress, 10000}}; // $100.00
    params.feeRate = 100000;
    params.spenderKey = spender;
    params.ddUtxos = MakeOutPoints(1);
    params.ddAmounts = {10000};
    params.feeUtxos = {COutPoint(uint256::ONE, 7)};
    params.feeAmounts = {10 * COIN}; // far more than the fee, so there is change
    return params;
}

TxBuilderRedeemParams BaseRedeemParams(const CKey& owner)
{
    TxBuilderRedeemParams params;
    uint256 collateralHash;
    collateralHash.SetHex("abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890");
    params.collateralOutpoint = COutPoint(collateralHash, 0);
    params.ddToRedeem = 10000; // $100.00
    params.path = RedemptionPath::NORMAL;
    params.ownerKey = owner;
    params.feeRate = 100000;
    params.ddUtxos = MakeOutPoints(1);
    params.ddAmounts = {10000};
    params.feeUtxos = {COutPoint(uint256::ONE, 7)};
    params.feeAmounts = {10 * COIN}; // far more than the fee, so there is change
    params.collateralAmount = 30000000000; // 300 DGB locked
    params.ddMinted = 10000;
    params.unlockHeight = 500; // already past, so the lock has expired
    return params;
}

// How many outputs pay this exact script, and what they add up to.
size_t CountOutputsPaying(const CMutableTransaction& tx, const CScript& script, CAmount& total)
{
    size_t count = 0;
    total = 0;
    for (const CTxOut& out : tx.vout) {
        if (out.scriptPubKey == script) {
            ++count;
            total += out.nValue;
        }
    }
    return count;
}

} // namespace

// A mint with real change and no change address must stop. Before this was
// fixed the builder made up a brand new key, used it once and threw it away,
// so the change was gone for good.
BOOST_AUTO_TEST_CASE(mint_without_change_address_fails)
{
    BigUtxoMintBuilder builder(Params(), TEST_HEIGHT, TEST_PRICE);

    TxBuilderMintParams params = BaseMintParams();
    BOOST_REQUIRE(!params.dgbChangeDest.has_value());

    const TxBuilderResult result = builder.BuildMintTransaction(params);

    BOOST_CHECK(!result.success);
    BOOST_CHECK(result.error.find("change address") != std::string::npos);
    // Nothing was built, so nothing can be signed or sent.
    BOOST_CHECK(result.tx.vout.empty());
    BOOST_CHECK(result.tx.vin.empty());
}

// With a normal bech32 change address the mint succeeds and the change goes to
// that exact address.
BOOST_AUTO_TEST_CASE(mint_pays_change_to_the_given_bech32_address)
{
    BigUtxoMintBuilder builder(Params(), TEST_HEIGHT, TEST_PRICE);

    const CKey changeKey = NewKey();
    TxBuilderMintParams params = BaseMintParams();
    params.dgbChangeDest = Bech32Destination(changeKey);

    const TxBuilderResult result = builder.BuildMintTransaction(params);

    BOOST_REQUIRE_MESSAGE(result.success, result.error);

    const CScript expected = GetScriptForDestination(*params.dgbChangeDest);
    CAmount paidToChange = 0;
    BOOST_CHECK_EQUAL(CountOutputsPaying(result.tx, expected, paidToChange), 1U);
    BOOST_CHECK_GT(paidToChange, 0);

    // Only the collateral may be a taproot output holding DGB. A second one
    // would make every node reject the mint.
    int taprootOutputsWithValue = 0;
    for (const CTxOut& out : result.tx.vout) {
        if (out.nValue > 0 && IsTaprootScript(out.scriptPubKey)) ++taprootOutputsWithValue;
    }
    BOOST_CHECK_EQUAL(taprootOutputsWithValue, 1);
}

// A taproot change address would give the mint two taproot outputs holding DGB.
// Nodes reject that, so the builder refuses before it builds anything.
BOOST_AUTO_TEST_CASE(mint_refuses_a_taproot_change_address)
{
    BigUtxoMintBuilder builder(Params(), TEST_HEIGHT, TEST_PRICE);

    const CKey changeKey = NewKey();
    TxBuilderMintParams params = BaseMintParams();
    params.dgbChangeDest = TaprootDestination(changeKey);

    const TxBuilderResult result = builder.BuildMintTransaction(params);

    BOOST_CHECK(!result.success);
    BOOST_CHECK(result.error.find("taproot") != std::string::npos);
    BOOST_CHECK(result.tx.vout.empty());
}

// An address the builder cannot turn into a script is no better than none.
BOOST_AUTO_TEST_CASE(mint_refuses_an_empty_change_address)
{
    BigUtxoMintBuilder builder(Params(), TEST_HEIGHT, TEST_PRICE);

    TxBuilderMintParams params = BaseMintParams();
    params.dgbChangeDest = CTxDestination{CNoDestination{}};

    const TxBuilderResult result = builder.BuildMintTransaction(params);

    BOOST_CHECK(!result.success);
    BOOST_CHECK(result.tx.vout.empty());
}

// A transfer with leftover DGB and no change address must stop. It used to send
// the change to the bech32 twin of the sender's DigiDollar key, which the
// wallet does not watch, so the balance looked like it had shrunk.
BOOST_AUTO_TEST_CASE(transfer_without_change_address_fails)
{
    BigUtxoTransferBuilder builder(Params(), TEST_HEIGHT, TEST_PRICE);

    const CKey spender = NewKey();
    const CKey recipient = NewKey();
    TxBuilderTransferParams params = BaseTransferParams(spender, recipient);
    BOOST_REQUIRE(!params.dgbChangeDest.has_value());

    const TxBuilderResult result = builder.BuildTransferTransaction(params);

    BOOST_CHECK(!result.success);
    BOOST_CHECK(result.error.find("change address") != std::string::npos);
    BOOST_CHECK(result.tx.vout.empty());

    // The old behaviour paid the change to this script. Prove it is gone.
    const CScript spenderTwin = GetScriptForDestination(WitnessV0KeyHash(spender.GetPubKey()));
    CAmount paidToTwin = 0;
    BOOST_CHECK_EQUAL(CountOutputsPaying(result.tx, spenderTwin, paidToTwin), 0U);
}

BOOST_AUTO_TEST_CASE(transfer_pays_change_to_the_given_address)
{
    BigUtxoTransferBuilder builder(Params(), TEST_HEIGHT, TEST_PRICE);

    const CKey spender = NewKey();
    const CKey recipient = NewKey();
    const CKey changeKey = NewKey();
    TxBuilderTransferParams params = BaseTransferParams(spender, recipient);
    params.dgbChangeDest = Bech32Destination(changeKey);

    const TxBuilderResult result = builder.BuildTransferTransaction(params);

    BOOST_REQUIRE_MESSAGE(result.success, result.error);

    const CScript expected = GetScriptForDestination(*params.dgbChangeDest);
    CAmount paidToChange = 0;
    BOOST_CHECK_EQUAL(CountOutputsPaying(result.tx, expected, paidToChange), 1U);
    BOOST_CHECK_EQUAL(paidToChange, params.feeAmounts[0] - result.totalFees);
}

// A redemption with leftover fee money and no address for it must stop. It used
// to build a taproot output from the untweaked owner key, which no wallet
// watches and no wallet can spend.
BOOST_AUTO_TEST_CASE(redeem_without_any_change_address_fails)
{
    BigUtxoRedeemBuilder builder(Params(), TEST_HEIGHT, TEST_PRICE);

    const CKey owner = NewKey();
    TxBuilderRedeemParams params = BaseRedeemParams(owner);
    BOOST_REQUIRE(!params.dgbChangeDest.has_value());
    BOOST_REQUIRE(!params.collateralDest.has_value());

    const TxBuilderResult result = builder.BuildRedemptionTransaction(params);

    BOOST_CHECK(!result.success);
    BOOST_CHECK(result.error.find("change address") != std::string::npos);
    BOOST_CHECK(result.tx.vout.empty());

    // The old behaviour paid the change to this script. Prove it is gone.
    const CScript ownerTaproot = GetScriptForDestination(WitnessV1Taproot(XOnlyPubKey(owner.GetPubKey())));
    CAmount paidToOwnerTaproot = 0;
    BOOST_CHECK_EQUAL(CountOutputsPaying(result.tx, ownerTaproot, paidToOwnerTaproot), 0U);
}

BOOST_AUTO_TEST_CASE(redeem_pays_change_to_the_given_address)
{
    BigUtxoRedeemBuilder builder(Params(), TEST_HEIGHT, TEST_PRICE);

    const CKey owner = NewKey();
    const CKey collateralKey = NewKey();
    const CKey changeKey = NewKey();
    TxBuilderRedeemParams params = BaseRedeemParams(owner);
    params.collateralDest = TaprootDestination(collateralKey);
    params.dgbChangeDest = Bech32Destination(changeKey);

    const TxBuilderResult result = builder.BuildRedemptionTransaction(params);

    BOOST_REQUIRE_MESSAGE(result.success, result.error);

    const CScript expected = GetScriptForDestination(*params.dgbChangeDest);
    CAmount paidToChange = 0;
    BOOST_CHECK_EQUAL(CountOutputsPaying(result.tx, expected, paidToChange), 1U);
    BOOST_CHECK_EQUAL(paidToChange, params.feeAmounts[0] - result.totalFees);

    // The returned collateral is its own output and still goes where the caller
    // asked.
    const CScript collateralScript = GetScriptForDestination(*params.collateralDest);
    CAmount returnedCollateral = 0;
    BOOST_CHECK_EQUAL(CountOutputsPaying(result.tx, collateralScript, returnedCollateral), 1U);
    BOOST_CHECK_EQUAL(returnedCollateral, params.collateralAmount);
}

// When the caller only gives an address for the returned collateral, the
// leftover fee money is allowed to go there too. That address belongs to
// whoever asked for the redemption, so no money is lost.
BOOST_AUTO_TEST_CASE(redeem_falls_back_to_the_collateral_address)
{
    BigUtxoRedeemBuilder builder(Params(), TEST_HEIGHT, TEST_PRICE);

    const CKey owner = NewKey();
    const CKey collateralKey = NewKey();
    TxBuilderRedeemParams params = BaseRedeemParams(owner);
    params.collateralDest = TaprootDestination(collateralKey);

    const TxBuilderResult result = builder.BuildRedemptionTransaction(params);

    BOOST_REQUIRE_MESSAGE(result.success, result.error);

    const CScript collateralScript = GetScriptForDestination(*params.collateralDest);
    CAmount paidToCollateralAddress = 0;
    BOOST_CHECK_EQUAL(CountOutputsPaying(result.tx, collateralScript, paidToCollateralAddress), 2U);
    BOOST_CHECK_EQUAL(paidToCollateralAddress, params.collateralAmount + params.feeAmounts[0] - result.totalFees);
}

// The builders only need a change address when change would actually be paid.
// If the leftover is smaller than the dust limit it becomes miner fee, and a
// mint with no change address still goes through.
BOOST_AUTO_TEST_CASE(mint_without_change_address_succeeds_when_there_is_no_change)
{
    // Each input is worth exactly the collateral plus a fee too small to leave
    // change behind, so the builder never needs a change address.
    class ExactFundsMintBuilder : public MintTxBuilder
    {
    public:
        ExactFundsMintBuilder(const CChainParams& params, int height, CAmount price)
            : MintTxBuilder(params, height, price) {}
        void SetUtxoValue(CAmount value) { m_value = value; }
        CAmount GetDGBFromUTXO(const COutPoint&) const override { return m_value; }
    private:
        CAmount m_value{0};
    };

    // First find out what this mint costs, using a build that is allowed to
    // have change.
    BigUtxoMintBuilder probe(Params(), TEST_HEIGHT, TEST_PRICE);
    const CKey changeKey = NewKey();
    TxBuilderMintParams probeParams = BaseMintParams();
    probeParams.dgbChangeDest = Bech32Destination(changeKey);
    const TxBuilderResult probeResult = probe.BuildMintTransaction(probeParams);
    BOOST_REQUIRE_MESSAGE(probeResult.success, probeResult.error);

    ExactFundsMintBuilder builder(Params(), TEST_HEIGHT, TEST_PRICE);
    builder.SetUtxoValue(probeResult.collateralRequired + probeResult.totalFees);

    TxBuilderMintParams params = BaseMintParams();
    params.ownerKey = probeParams.ownerKey;
    params.utxos = MakeOutPoints(1);
    BOOST_REQUIRE(!params.dgbChangeDest.has_value());

    const TxBuilderResult result = builder.BuildMintTransaction(params);

    BOOST_REQUIRE_MESSAGE(result.success, result.error);
    // Collateral, DigiDollar token and the OP_RETURN, and nothing else.
    BOOST_CHECK_EQUAL(result.tx.vout.size(), 3U);
}

BOOST_AUTO_TEST_SUITE_END()
