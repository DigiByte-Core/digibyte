//
// Created by cl on 2021/1/6.
//

#include "odo_sha256_param_gen.h"

int compare_qsort(const void* a, const void* b)
{
    return (*(uint32_t*)a - *(uint32_t*)b);
}

ODO_SHA256_PARAM_GEN::ODO_SHA256_PARAM_GEN(uint64_t key)
{
    this->key = key;
    uint32_t* primes = (uint32_t*)malloc(TABLE_SIZE * sizeof(uint32_t));
    try {
        this->get_first_table_size_primes(primes);
        this->create_decimals_of_square_root_of_primes(primes);
        this->create_decimals_of_cube_roots_of_primes(primes);
        this->generate();
    } catch (...) {
        free(primes);
        throw;
    }
    free(primes);
}

ODO_SHA256_PARAM_GEN::~ODO_SHA256_PARAM_GEN()
{
    if(this->sqrts != NULL){
        free(this->sqrts);
    }
    if(this->curts != NULL){
        free(this->curts);
    }
}

void ODO_SHA256_PARAM_GEN::check_safe(uint32_t table[TABLE_SIZE])
{
    uint32_t* table_tmp = (uint32_t*)malloc(TABLE_SIZE * sizeof(uint32_t));
    for (uint32_t i = 0; i < TABLE_SIZE; i++) {
        table_tmp[i] = table[i];
    }
    sort(table_tmp, table_tmp + TABLE_SIZE);

    uint32_t i = 0;
    for (; i < TABLE_SIZE - 1; i++) {
        if (table_tmp[i] == table_tmp[i + 1])
            break;
    }

    free(table_tmp);
    if (i < TABLE_SIZE - 1) {
        assert(1 > 2 && "check safe, failed");
    }
}

CBigNum ODO_SHA256_PARAM_GEN::get_m()
{
    CBigNum multiplier(1);
    multiplier <<= 108;
    multiplier -= 59;

    CBigNum multiplicand(1);
    multiplicand <<= 126;
    multiplicand -= 335;

    return multiplier * multiplicand;
}

bool ODO_SHA256_PARAM_GEN::is_prime(uint32_t n)
{
    uint32_t q = (uint32_t)sqrt((double)n);
    for (uint32_t i = 2; i <= q; i++) {
        if (n % i == 0) return false;
    }
    return true;
}

void ODO_SHA256_PARAM_GEN::get_first_table_size_primes(uint32_t primes_out[TABLE_SIZE])
{
    uint32_t primes_len = 0;
    for (uint32_t i = 2;; i++) {
        if (this->is_prime(i)) {
            primes_out[primes_len++] = i;
            if (primes_len >= TABLE_SIZE) {
                break;
            }
        }
    }
}

void ODO_SHA256_PARAM_GEN::create_decimals_of_square_root_of_primes(const uint32_t primes[TABLE_SIZE])
{
    uint32_t sqrts_len = 0;
    long double md = 1;

    this->sqrts = (uint32_t*)malloc(TABLE_SIZE * sizeof(uint32_t));
    for (uint32_t i = 0; i < TABLE_SIZE; i++) {
        this->sqrts[sqrts_len++] = floorl(powl(2, 32) * modfl(sqrtl(primes[i]), &md));
    }
    this->check_safe(this->sqrts);
}

void ODO_SHA256_PARAM_GEN::create_decimals_of_cube_roots_of_primes(const uint32_t primes[TABLE_SIZE])
{
    uint32_t curts_len = 0;
    long double md = 1;

    this->curts = (uint32_t*)malloc(TABLE_SIZE * sizeof(uint32_t));
    for (uint32_t i = 0; i < TABLE_SIZE; i++) {
        this->curts[curts_len++] = floorl(powl(2, 32) * modfl(cbrtl(primes[i]), &md));
    }
    this->check_safe(this->curts);
}

void ODO_SHA256_PARAM_GEN::blum_blum_shub_mod(uint64_t seed, const uint32_t table[TABLE_SIZE], uint32_t count, uint32_t* out)
{
    CBigNum m_big = this->get_m();

    CBigNum seed_big(seed), two_big(2), zero_big(0);
    seed_big = seed_big % m_big;

    uint32_t out_len = 0;
    std::map<uint32_t, uint32_t> used_map;

    while (out_len < count) {
        uint32_t bits = 0;
        for (uint32_t i = 0; i < TABLE_SIZE_BITS; i++) {
            seed_big = (seed_big * seed_big) % m_big;
            bits = bits * 2 + (seed_big % two_big).getuint();
        }
        std::map<uint32_t, uint32_t>::iterator is_used = used_map.find(bits);
        if (is_used == used_map.end()) {
            used_map[bits] = 1;
            out[out_len++] = table[bits];
        }
    }
}

void ODO_SHA256_PARAM_GEN::gen_k256(uint64_t t)
{
    blum_blum_shub_mod(t, this->curts, 64, this->k256);
}

void ODO_SHA256_PARAM_GEN::gen_sha256_initial_hash_value(uint64_t t)
{
    blum_blum_shub_mod(t, this->sqrts, 8, this->sha256_initial_hash_value);
}

void ODO_SHA256_PARAM_GEN::generate()
{
    uint64_t next_t = ceill((long double)(T + this->key * EPOCH_PERIOD) / EPOCH_PERIOD);
    gen_k256(next_t);
    gen_sha256_initial_hash_value(next_t);
}