// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_CONSENSUS_DD_MINT_ANCHOR_H
#define DIGIBYTE_CONSENSUS_DD_MINT_ANCHOR_H

#include <consensus/amount.h>

#include <vector>

namespace DigiDollar {
namespace MintAnchor {

/**
 * Lower median of committed oracle bundle prices: sorted ascending, element
 * at index (n-1)/2 (for even n this is the lower of the two middle values —
 * deterministic, no averaging). Empty input returns 0, which the mint
 * volatility gate treats as "no anchor => pass" (bootstrap / oracle-drought
 * self-expiry).
 *
 * Takes the vector by value because it sorts in place. Integer-only: this
 * feeds block validity, so no floating point.
 */
CAmount LowerMedian(std::vector<CAmount> values);

} // namespace MintAnchor
} // namespace DigiDollar

#endif // DIGIBYTE_CONSENSUS_DD_MINT_ANCHOR_H
