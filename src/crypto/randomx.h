// Copyright (c) 2021 barrystyle
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_RANDOMX_H
#define DIGIBYTE_RANDOMX_H

#include <crypto/randomx/randomx.h>
#include <crypto/seedman.h>
#include <uint256.h>

class RandomXManager;
class uint256;
class CBlockIndex;
class CBlockHeader;

extern SeedManager seedmanager;
extern RandomXManager rxmanager;

class RandomXManager {

public:
    uint256 seed;

private:
    randomx_flags flags;
    randomx_vm* vm;
    randomx_cache* cache;

public:
    RandomXManager()
    {
        seed = uint256();
        vm = nullptr;
        cache = nullptr;
    }

    void CacheInit();
    void VmInit();
    void Shutoff();
    bool HasSeedChanged(int height);
    void UpdateSeed(int height);
    void Hash(const char* input, char* output, int height);
};

#endif // DIGIBYTE_RANDOMX_H
