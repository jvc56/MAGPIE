#ifndef RAND_H
#define RAND_H

#include <stdint.h>

void seed_random(uint64_t seed);
uint64_t rand_uint64();

#endif