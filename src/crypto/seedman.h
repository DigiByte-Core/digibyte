// Copyright (c) 2021 barrystyle
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_SEEDMAN_H
#define DIGIBYTE_SEEDMAN_H

#include <chain.h>
#include <primitives/block.h>
#include <uint256.h>
#include <util/system.h>
#include <validation.h>

class CBlockHeader;
class uint256;

class SeedManager {

private:
    const int epoch_length = 5;

public:
    int GetEpochNumber(int height);
    uint256 GetSeedForHeight(int height);
    uint256 CalculateEpochSeed(int epoch_number);
};

#endif // DIGIBYTE_SEEDMAN_H
