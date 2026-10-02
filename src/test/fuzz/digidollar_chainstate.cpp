// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/digidollar_state.h>
#include <consensus/volatility.h>
#include <streams.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>

#include <algorithm>
#include <cassert>
#include <limits>

FUZZ_TARGET(dd_chainstate_health)
{
    FuzzedDataProvider provider(buffer.data(), buffer.size());
    DigiDollar::ChainstateHealth state;
    state.genesis_hash = uint256::ONE;
    state.best_block = uint256::ONE;
    state.history_checked = provider.ConsumeBool();

    for (int i = 0; i < 100 && provider.remaining_bytes(); ++i) {
        const auto before = state;
        const bool add = provider.ConsumeBool();
        const CAmount principal = provider.ConsumeBool() ?
            provider.ConsumeIntegralInRange<CAmount>(1, 10000000) : provider.ConsumeIntegral<CAmount>();
        const CAmount value = provider.ConsumeBool() ?
            provider.ConsumeIntegralInRange<CAmount>(1, MAX_MONEY) : provider.ConsumeIntegral<CAmount>();
        if (add) {
            if (state.AddVault(principal, value)) {
                auto inverse = state;
                assert(inverse.RemoveVault(principal, value));
                assert(inverse == before);
            } else {
                assert(state == before);
            }
        } else {
            if (state.RemoveVault(principal, value)) {
                auto inverse = state;
                assert(inverse.AddVault(principal, value));
                assert(inverse == before);
            } else {
                assert(state == before);
            }
        }
        assert(state.IsValid());
        assert(state.Matches(before.genesis_hash, before.best_block));
        assert(state.history_checked == before.history_checked);
        CDataStream encoded(SER_DISK, 0);
        encoded << state;
        DigiDollar::ChainstateHealth restored;
        encoded >> restored;
        assert(encoded.empty());
        assert(restored == state);
    }

    // Closing all remaining liabilities must restore the verified empty state.
    if (state.active_vaults == 1) {
        assert(state.RemoveVault(state.open_vault_principal, state.collateral));
        assert(state.IsValid());
        assert(state.open_vault_principal == 0 && state.collateral == 0 && state.active_vaults == 0);
    }
}

FUZZ_TARGET(dd_mint_reference_price)
{
    FuzzedDataProvider provider(buffer.data(), buffer.size());
    using namespace DigiDollar::Volatility;
    MintReference reference;
    reference.ready = true;
    reference.sample_count = provider.ConsumeIntegralInRange<size_t>(1, MINT_REFERENCE_SAMPLES);
    reference.price_micro_usd = provider.ConsumeIntegralInRange<CAmount>(1, std::numeric_limits<CAmount>::max());
    const CAmount quote = provider.ConsumeIntegralInRange<CAmount>(1, std::numeric_limits<CAmount>::max());
    const auto result = EvaluateMintPrice(quote, reference);
    assert(result.ready && result.quote_available);
    assert(result.deviation_bps >= 0);
    assert(!EvaluateMintPrice(reference.price_micro_usd, reference).restricted);

    // The decision is a ratio: scaling both prices must preserve it exactly.
    const CAmount largest = std::max(quote, reference.price_micro_usd);
    const CAmount scale = provider.ConsumeIntegralInRange<CAmount>(1, std::numeric_limits<CAmount>::max() / largest);
    auto scaled = reference;
    scaled.price_micro_usd *= scale;
    const auto scaled_result = EvaluateMintPrice(quote * scale, scaled);
    assert(scaled_result.restricted == result.restricted);
    assert(scaled_result.deviation_bps == result.deviation_bps);

    auto unavailable = reference;
    unavailable.ready = false;
    const auto missing = EvaluateMintPrice(quote, unavailable);
    assert(!missing.ready && !missing.restricted);
    auto empty = reference;
    empty.sample_count = 0;
    empty.price_micro_usd = 0;
    const auto no_history = EvaluateMintPrice(quote, empty);
    assert(no_history.ready && !no_history.restricted);
    assert(!EvaluateMintPrice(0, reference).quote_available);
}
