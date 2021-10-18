// Copyright (c) 2021 barrystyle
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <crypto/seedman.h>

int SeedManager::GetEpochNumber(int height)
{
    return height ? height / epoch_length : 0;
}

uint256 SeedManager::GetSeedForHeight(int height)
{
    return CalculateEpochSeed(GetEpochNumber(height));
}

uint256 SeedManager::CalculateEpochSeed(int epoch_number)
{
    uint256 ret{};
    for (uint32_t i = 0; i < epoch_number; ++i) {
         ret = SerializeHash(ret);
    }
    return ret;
}
