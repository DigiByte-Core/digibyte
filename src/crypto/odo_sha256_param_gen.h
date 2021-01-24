//
// Created by cl on 2021/1/6.
//

#ifndef ODO_SHA256_PARAM_GEN_H
#define ODO_SHA256_PARAM_GEN_H

#include <bignum.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
#include <algorithm>
using namespace std;

class ODO_SHA256_PARAM_GEN
{
private:
    static const uint32_t TABLE_SIZE_BITS = 14;
    static const uint32_t TABLE_SIZE = pow(2, TABLE_SIZE_BITS);
    static const uint64_t EPOCH_PERIOD = 864000;
    static const uint64_t T = 1609653714;

    uint32_t *sqrts;
    uint32_t *curts;

    uint32_t key;

    CBigNum get_m();
    bool is_prime(uint32_t n);
    void check_safe(uint32_t table[TABLE_SIZE]);
    void get_first_table_size_primes(uint32_t primes_out[TABLE_SIZE]);
    void create_decimals_of_square_root_of_primes(const uint32_t primes[TABLE_SIZE]);
    void create_decimals_of_cube_roots_of_primes(const uint32_t primes[TABLE_SIZE]);
    void blum_blum_shub_mod(uint64_t seed, const uint32_t table[TABLE_SIZE], uint32_t count, uint32_t* out);
    void gen_k256(uint64_t t);
    void gen_sha256_initial_hash_value(uint64_t t);
    void generate();

public:
    ODO_SHA256_PARAM_GEN(uint64_t key);
    ~ODO_SHA256_PARAM_GEN();

    uint32_t sha256_initial_hash_value[8];
    uint32_t k256[64];
};


#endif //ODO_SHA256_PARAM_GEN_H
