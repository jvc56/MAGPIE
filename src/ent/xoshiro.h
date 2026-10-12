#ifndef XOSHIRO_H
#define XOSHIRO_H

#include <stddef.h>
#include <stdint.h>

#define XOSHIRO_MAX UINT64_C(18446744073709551615)

typedef struct XoshiroPRNG XoshiroPRNG;
size_t prng_allocation_size(void);

XoshiroPRNG *prng_create(uint64_t seed);
void prng_destroy(XoshiroPRNG *prng);
void prng_copy(XoshiroPRNG *dst, const XoshiroPRNG *src);

void prng_seed(XoshiroPRNG *prng, uint64_t seed);
uint64_t prng_next(XoshiroPRNG *prng);
// The value prng_next would return after `ahead` further calls, without
// advancing the generator. It works on a copy of the state on the stack, so it
// allocates nothing.
uint64_t prng_peek(const XoshiroPRNG *prng, uint64_t ahead);
void prng_jump(XoshiroPRNG *prng);
uint64_t prng_get_random_number(XoshiroPRNG *prng, uint64_t n);

#endif