//
// Created by cl on 2021/1/6.
//

#ifndef ODO_SHA256_PARAM_GEN_H
#define ODO_SHA256_PARAM_GEN_H

#include "bigint.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

void generate(uint64_t key, uint32_t h256_out[8], uint32_t k256_out[64]);

#endif //ODO_SHA256_PARAM_GEN_H
