#ifndef XOSHIRO_H
#define XOSHIRO_H

#include <stdint.h>

#define XOSHIRO_MAX UINT64_C(18446744073709551615)

typedef struct XoshiroPRNG XoshiroPRNG;

XoshiroPRNG *prng_create(uint64_t seed);
void prng_destroy(XoshiroPRNG *prng);
void prng_copy(XoshiroPRNG *dst, const XoshiroPRNG *src);

void prng_seed(XoshiroPRNG *prng, uint64_t seed);
uint64_t prng_next(XoshiroPRNG *prng);
void prng_jump(XoshiroPRNG *prng);
uint64_t prng_get_random_number(XoshiroPRNG *prng, uint64_t n);

#endif