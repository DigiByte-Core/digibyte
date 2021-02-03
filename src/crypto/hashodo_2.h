// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2018 The DigiByte developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef HASH_ODO_2
#define HASH_ODO_2

#include "odo_sha256_param_gen.h"
#include "odocrypt.h"
#include "scrypt.h"
#include "sha256_2.h"
#include "uint256.h"

template <typename T1>
inline uint256 HashOdo2(const T1 pbegin, const T1 pend, uint32_t key)
{
    char cipher[OdoCrypt::DIGEST_SIZE] = {};
    uint256 hash;

    size_t len = (pend - pbegin) * sizeof(pbegin[0]);
    assert(len <= OdoCrypt::DIGEST_SIZE);
    memcpy(cipher, static_cast<const void*>(&pbegin[0]), len);

    OdoCrypt(key).Encrypt(cipher, cipher);

    uint32_t h256[8], k256[64];
    generate(key, h256, k256);
    CSHA256_2 sha256(h256, k256);

    uint8_t ucipher[OdoCrypt::DIGEST_SIZE] = {};
    for (size_t i = 0; i < OdoCrypt::DIGEST_SIZE; i++) {
        ucipher[i] = cipher[i];
    }
    sha256.Write(ucipher, (size_t)(OdoCrypt::DIGEST_SIZE));

    uint8_t sha256_out[CSHA256_2::OUTPUT_SIZE];
    sha256.Finalize(sha256_out);

    memcpy(&hash, sha256_out, hash.size());

    return hash;
}

#endif
