#ifndef RNG_H
#define RNG_H

#include <stdint.h>

typedef struct RNG RNG;

typedef struct RNGArgs {
  int num_samples;
  double *samples;
  uint64_t seed;
  int num_rngs;
} RNGArgs;

RNG *rng_create(RNGArgs *args);
void rng_destroy(RNG *rng);
double rng_normal(RNG *rng, double mean, double stddev, int rng_index);

#endif
