// Copyright (c) 2025 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <oracle/mock_oracle.h>

#include <chain.h>
#include <chainparams.h>
#include <crypto/sha256.h>
#include <hash.h>
#include <logging.h>
#include <oracle/bundle_manager.h>
#include <oracle/musig2_aggregator.h>
#include <util/strencodings.h>
#include <node/chainstate.h>
#include <random.h>
#include <util/time.h>
#include <validation.h>

#include <secp256k1.h>
#include <secp256k1_musig.h>
#include <secp256k1_schnorrsig.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

// Global instance pointer
MockOracleManager* MockOracleManager::instance = nullptr;

namespace {

std::array<unsigned char, 32> RegtestOracleSecret(uint8_t oracle_id)
{
    const std::string seed = "digibyte_regtest_oracle_" + std::to_string(oracle_id);
    uint256 hash;
    CSHA256().Write(reinterpret_cast<const unsigned char*>(seed.data()), seed.size()).Finalize(hash.begin());

    std::array<unsigned char, 32> secret{};
    std::memcpy(secret.data(), hash.begin(), secret.size());
    return secret;
}

bool SignRegtestMuSig2Bundle(COracleBundle& bundle, const std::vector<uint8_t>& oracle_ids)
{
    if (oracle_ids.empty()) return false;

    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    if (!ctx) return false;

    const size_t n_signers = oracle_ids.size();
    std::vector<std::array<unsigned char, 32>> seckeys(n_signers);
    std::vector<secp256k1_keypair> keypairs(n_signers);
    std::vector<secp256k1_pubkey> pubkeys(n_signers);

    for (size_t i = 0; i < n_signers; ++i) {
        seckeys[i] = RegtestOracleSecret(oracle_ids[i]);
        if (!secp256k1_keypair_create(ctx, &keypairs[i], seckeys[i].data()) ||
            !secp256k1_keypair_pub(ctx, &pubkeys[i], &keypairs[i])) {
            secp256k1_context_destroy(ctx);
            return false;
        }
    }

    std::vector<const secp256k1_pubkey*> pubkey_ptrs(n_signers);
    for (size_t i = 0; i < n_signers; ++i) {
        pubkey_ptrs[i] = &pubkeys[i];
    }

    secp256k1_xonly_pubkey agg_pk{};
    secp256k1_musig_keyagg_cache cache{};
    if (!secp256k1_musig_pubkey_agg(ctx, &agg_pk, &cache, pubkey_ptrs.data(), n_signers)) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    std::vector<secp256k1_musig_secnonce> secnonces(n_signers);
    std::vector<secp256k1_musig_pubnonce> pubnonces(n_signers);
    for (size_t i = 0; i < n_signers; ++i) {
        unsigned char session_rand[32];
        GetStrongRandBytes(Span{session_rand, 32});
        if (!secp256k1_musig_nonce_gen(ctx, &secnonces[i], &pubnonces[i],
                                       session_rand, seckeys[i].data(), &pubkeys[i],
                                       nullptr, &cache, nullptr)) {
            secp256k1_context_destroy(ctx);
            return false;
        }
    }

    std::vector<const secp256k1_musig_pubnonce*> nonce_ptrs(n_signers);
    for (size_t i = 0; i < n_signers; ++i) {
        nonce_ptrs[i] = &pubnonces[i];
    }

    secp256k1_musig_aggnonce aggnonce{};
    if (!secp256k1_musig_nonce_agg(ctx, &aggnonce, nonce_ptrs.data(), n_signers)) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    const uint256 msg_hash = ComputeOracleBundleHash(bundle);
    unsigned char msg32[32];
    std::memcpy(msg32, msg_hash.begin(), sizeof(msg32));

    secp256k1_musig_session session{};
    if (!secp256k1_musig_nonce_process(ctx, &session, &aggnonce, msg32, &cache)) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    std::vector<secp256k1_musig_partial_sig> partial_sigs(n_signers);
    std::vector<const secp256k1_musig_partial_sig*> partial_ptrs(n_signers);
    for (size_t i = 0; i < n_signers; ++i) {
        if (!secp256k1_musig_partial_sign(ctx, &partial_sigs[i], &secnonces[i],
                                          &keypairs[i], &cache, &session)) {
            secp256k1_context_destroy(ctx);
            return false;
        }
        partial_ptrs[i] = &partial_sigs[i];
    }

    bundle.participation_bitmap = MuSig2OracleAggregator::EncodeBitmap(
        oracle_ids, static_cast<uint16_t>(Params().GetConsensus().nOracleTotalOracles));
    bundle.aggregate_sig.assign(64, 0);
    if (!secp256k1_musig_partial_sig_agg(ctx, bundle.aggregate_sig.data(),
                                         &session, partial_ptrs.data(), n_signers)) {
        secp256k1_context_destroy(ctx);
        return false;
    }

    const bool verifies = secp256k1_schnorrsig_verify(ctx, bundle.aggregate_sig.data(), msg32, 32, &agg_pk);
    secp256k1_context_destroy(ctx);
    return verifies;
}

} // namespace

MockOracleManager::MockOracleManager()
    : mockPriceMicroUSD(6500),   // Default: $0.0065 per DGB = 6500 micro-USD (realistic DGB price)
      lastUpdateHeight(0),
      enabled(true)
{
    InitTestKeys();
}

void MockOracleManager::InitTestKeys()
{
    // Generate deterministic test oracle private keys from SHA256("digibyte_regtest_oracle_N")
    // 7 oracles for regtest (matches testnet 4-of-7 consensus)
    for (uint32_t i = 0; i < 7; i++) {
        std::string seed = "digibyte_regtest_oracle_" + std::to_string(i);
        uint256 hash;
        CSHA256().Write((const unsigned char*)seed.data(), seed.size()).Finalize(hash.begin());

        CKey key;
        key.Set(hash.begin(), hash.end(), true);
        if (key.IsValid()) {
            testOracleKeys[i] = key;
            LogPrint(BCLog::DIGIDOLLAR, "MockOracleManager: Initialized test key for oracle %d (pubkey=%s)\n",
                     i, HexStr(key.GetPubKey()));
        } else {
            LogPrintf("MockOracleManager: WARNING - Failed to create test key for oracle %d\n", i);
        }
    }
}

CKey MockOracleManager::GetTestKey(uint32_t oracle_id) const
{
    LOCK(cs_price);
    auto it = testOracleKeys.find(oracle_id);
    if (it != testOracleKeys.end()) {
        return it->second;
    }
    return CKey(); // Invalid key
}

MockOracleManager& MockOracleManager::GetInstance()
{
    if (!instance) {
        instance = new MockOracleManager();
    }
    return *instance;
}

CAmount MockOracleManager::GetCurrentPrice() const
{
    // SECURITY (DGB-SEC-005): Runtime guard — reject on non-REGTEST networks
    if (Params().GetChainType() != ChainType::REGTEST) {
        return 0;
    }
    LOCK(cs_price);
    return mockPriceMicroUSD;
}

void MockOracleManager::SetMockPrice(CAmount price_micro_usd)
{
    SetMockPrice(price_micro_usd, -1);
}

void MockOracleManager::SetMockPrice(CAmount price_micro_usd, int64_t update_height)
{
    // SECURITY (DGB-SEC-005): Runtime guard — mock oracle must only operate in REGTEST
    if (Params().GetChainType() != ChainType::REGTEST) {
        LogPrintf("MockOracleManager: SECURITY - SetMockPrice rejected on non-REGTEST network\n");
        return;
    }

    LOCK(cs_price);

    // Validate price is reasonable
    if (price_micro_usd <= 0) {
        LogPrintf("MockOracleManager: Invalid price %lld micro-USD, ignoring\n", price_micro_usd);
        return;
    }

    // Price is in micro-USD (1,000,000 = $1.00)
    // Range: $0.0001 per DGB (100 micro-USD) to $1000 per DGB (1,000,000,000 micro-USD)
    const CAmount MIN_PRICE = 100;              // 100 micro-USD = $0.0001 per DGB (minimum reasonable)
    const CAmount MAX_PRICE = 1000000000;       // 1,000,000,000 micro-USD = $1000 per DGB (maximum reasonable)

    if (price_micro_usd < MIN_PRICE || price_micro_usd > MAX_PRICE) {
        LogPrintf("MockOracleManager: Price %lld micro-USD out of reasonable range [%lld, %lld], clamping\n",
                  price_micro_usd, MIN_PRICE, MAX_PRICE);
        price_micro_usd = std::max(MIN_PRICE, std::min(MAX_PRICE, price_micro_usd));
    }

    mockPriceMicroUSD = price_micro_usd;

    if (update_height >= 0) {
        lastUpdateHeight = update_height;
    }

    LogPrint(BCLog::DIGIDOLLAR, "MockOracleManager: Price updated to %lld micro-USD ($%.6f per DGB)\n",
              mockPriceMicroUSD, static_cast<double>(mockPriceMicroUSD) / 1000000.0);
}

bool MockOracleManager::IsEnabled() const
{
    LOCK(cs_price);
    return enabled;
}

void MockOracleManager::SetEnabled(bool enable)
{
    LOCK(cs_price);
    enabled = enable;
    LogPrintf("MockOracleManager: %s\n",
              enable ? "Enabled" : "Disabled");
}

int64_t MockOracleManager::GetLastUpdateHeight() const
{
    LOCK(cs_price);
    return lastUpdateHeight;
}

COracleBundle MockOracleManager::CreateMockMuSig2Bundle(int height, int64_t block_time)
{
    LOCK(cs_price);

    COracleBundle bundle(GetCurrentEpoch(height));
    bundle.version = 3;
    bundle.median_price_micro_usd = mockPriceMicroUSD;
    bundle.timestamp = block_time > 0 ? block_time : GetTime();

    std::vector<uint8_t> oracle_ids;
    const int required = std::max(1, Params().GetConsensus().nOracleConsensusRequired);
    for (uint8_t id = 0; id < testOracleKeys.size() && static_cast<int>(oracle_ids.size()) < required; ++id) {
        oracle_ids.push_back(id);
    }

    if (static_cast<int>(oracle_ids.size()) < required ||
        !SignRegtestMuSig2Bundle(bundle, oracle_ids)) {
        LogPrintf("MockOracleManager: Failed to create regtest MuSig2 oracle bundle for height %d\n",
                  height);
        return COracleBundle(GetCurrentEpoch(height));
    }

    LogPrint(BCLog::DIGIDOLLAR, "MockOracleManager: Created regtest MuSig2 bundle for height %d epoch %d with %zu signers, price %lld micro-USD\n",
              height, bundle.epoch, oracle_ids.size(), mockPriceMicroUSD);
    return bundle;
}

void MockOracleManager::SimulateVolatility(int percentChange)
{
    SimulateVolatility(percentChange, -1);
}

void MockOracleManager::SimulateVolatility(int percentChange, int64_t update_height)
{
    LOCK(cs_price);

    if (percentChange == 0) {
        return;
    }

    // Calculate new price based on percentage change
    CAmount oldPrice = mockPriceMicroUSD;
    CAmount change = (mockPriceMicroUSD * percentChange) / 100;
    CAmount newPrice = mockPriceMicroUSD + change;

    // Ensure price stays in reasonable range (micro-USD: 1,000,000 = $1.00)
    const CAmount MIN_PRICE = 100;              // 100 micro-USD = $0.0001 per DGB
    const CAmount MAX_PRICE = 1000000000;       // 1,000,000,000 micro-USD = $1000 per DGB

    newPrice = std::max(MIN_PRICE, std::min(MAX_PRICE, newPrice));

    mockPriceMicroUSD = newPrice;

    if (update_height >= 0) {
        lastUpdateHeight = update_height;
    }

    LogPrint(BCLog::DIGIDOLLAR, "MockOracleManager: Simulated %d%% volatility: %lld -> %lld micro-USD ($%.6f -> $%.6f per DGB)\n",
              percentChange, oldPrice, mockPriceMicroUSD,
              static_cast<double>(oldPrice) / 1000000.0, static_cast<double>(mockPriceMicroUSD) / 1000000.0);
}

void MockOracleManager::Reset()
{
    LOCK(cs_price);

    mockPriceMicroUSD = 6500;  // Reset to 6500 micro-USD = $0.0065 per DGB (realistic DGB price)
    lastUpdateHeight = 0;
    enabled = true;

    LogPrintf("MockOracleManager: Reset to default state (price: %lld micro-USD = $%.6f per DGB)\n",
              mockPriceMicroUSD, static_cast<double>(mockPriceMicroUSD) / 1000000.0);
}
