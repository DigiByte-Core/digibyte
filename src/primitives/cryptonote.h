// Copyright (c) 2021 barrystyle
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_CRYPTONOTE_H
#define DIGIBYTE_CRYPTONOTE_H

#include <crypto/randomx/randomx.h>
#include <primitives/block.h>

class RandomXManager;

uint256 serialize_cryptonote(const CBlockHeader* block, RandomXManager* rxinstance, int height);

#endif // DIGIBYTE_CRYPTONOTE_H
