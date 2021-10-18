// Copyright (c) 2021 barrystyle
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <crypto/randomx.h>

#include <chainparams.h>
#include <crypto/randomx/randomx.h>
#include <crypto/seedman.h>
#include <primitives/block.h>
#include <primitives/cryptonote.h>
#include <sync.h>

SeedManager seedmanager;
RandomXManager rxmanager;

void RandomXManager::CacheInit()
{
    if (!cache) {
        flags = randomx_get_flags();
        cache = randomx_alloc_cache(flags);
    }

    randomx_init_cache(cache, &seed, 32);
}

void RandomXManager::VmInit()
{
    if (vm) {
        randomx_destroy_vm(vm);
    }

    vm = randomx_create_vm(flags, cache, nullptr);
}

void RandomXManager::Shutoff()
{
    if (vm) {
        randomx_destroy_vm(vm);
        vm = nullptr;
    }

    if (cache) {
        randomx_release_cache(cache);
        cache = nullptr;
    }
}

bool RandomXManager::HasSeedChanged(int height)
{
    uint256 seed_for_height = seedmanager.GetSeedForHeight(height);
    if (seed != seed_for_height) {
        return true;
    }

    return false;
}

void RandomXManager::UpdateSeed(int height)
{
    seed = seedmanager.GetSeedForHeight(height);
    LogPrintf("seed changed to %s at height %d\n", seed.ToString(), height);
}

void RandomXManager::Hash(const char* input, char* output, int height)
{
    if (!cache) {
        CacheInit();
    }

    if (!vm) {
        VmInit();
    }

    bool refresh = HasSeedChanged(height);

    if (refresh) {
        UpdateSeed(height);
        Shutoff();
        CacheInit();
        VmInit();
    }

    randomx_calculate_hash(vm, input, 76, output);
}
