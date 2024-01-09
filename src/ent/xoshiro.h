#ifndef XOSHIRO_H
#define XOSHIRO_H

#include <stdint.h>

typedef struct XoshiroPRNG XoshiroPRNG;

XoshiroPRNG *prng_create(uint64_t seed);
void prng_destroy(XoshiroPRNG *x);
void prng_copy(XoshiroPRNG *dst, const XoshiroPRNG *src);

void prng_seed(XoshiroPRNG *x, uint64_t seed);
void prng_jump(XoshiroPRNG *x);
uint64_t prng_get_random_number(XoshiroPRNG *x, uint64_t n);

#endif