// Copyright (c) 2021 barrystyle
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <crypto/randomx.h>
#include <crypto/randomx/randomx.h>
#include <crypto/sha512.h>
#include <primitives/block.h>
#include <sync.h>

uint256 serialize_cryptonote(const CBlockHeader* block, RandomXManager* rxinstance, int height)
{
    CBlockHeader blockNoNonce;
    blockNoNonce.nVersion = block->nVersion;
    blockNoNonce.hashPrevBlock = block->hashPrevBlock;
    blockNoNonce.hashMerkleRoot = block->hashMerkleRoot;
    blockNoNonce.nTime = block->nTime;
    blockNoNonce.nBits = block->nBits;
    blockNoNonce.nNonce = (uint32_t)0;

    unsigned char prehash[64];
    memset(prehash, 0, sizeof(prehash));
    CSHA512 hasher;
    hasher.Write((const unsigned char*)&blockNoNonce, 80);
    hasher.Finalize(prehash);

    unsigned char cryptonoteHeader[76];
    memset(cryptonoteHeader, 0, sizeof(cryptonoteHeader));
    memcpy(cryptonoteHeader, (unsigned char*)&prehash, 64);
    memcpy(cryptonoteHeader + 39, (unsigned char*)&block->nNonce, 4);

    uint256 powhash {};
    rxinstance->Hash((const char*)cryptonoteHeader, (char*)&prehash, height);
    memcpy(&powhash, &prehash, 32);
    return powhash;
}
