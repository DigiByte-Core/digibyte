// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/dd_mint_anchor.h>

#include <algorithm>

namespace DigiDollar {
namespace MintAnchor {

CAmount LowerMedian(std::vector<CAmount> values)
{
    if (values.empty()) {
        return 0;
    }
    std::sort(values.begin(), values.end());
    return values[(values.size() - 1) / 2];
}

} // namespace MintAnchor
} // namespace DigiDollar
