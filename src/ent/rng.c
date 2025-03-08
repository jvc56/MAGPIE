#include "rng.h" // Include the xoshiro PRNG header

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "xoshiro.h"

#include "../util/log.h"
#include "../util/string_util.h"
#include "../util/util.h"

typedef enum { RNG_XOSHIRO, RNG_FILE } rng_t;

struct RNG {
  rng_t type;
  int num_rngs;
  XoshiroPRNG **xoshiro_prngs;
  uint64_t num_samples;
  uint64_t *indexes;
  uint64_t *samples;
};

RNG *rng_create(RNGArgs *args) {
  RNG *rng = malloc_or_die(sizeof(RNG));
  rng->num_rngs = args->num_rngs;
  rng->type = RNG_XOSHIRO;
  if (args->num_samples > 0) {
    rng->type = RNG_FILE;
  }
  switch (rng->type) {
  case RNG_XOSHIRO:
    rng->xoshiro_prngs = malloc_or_die(args->num_rngs * sizeof(XoshiroPRNG *));
    for (int i = 0; i < args->num_rngs; i++) {
      rng->xoshiro_prngs[i] = prng_create(args->seed);
    }
    break;
  case RNG_FILE:
    rng->num_samples = args->num_samples;
    rng->indexes = calloc_or_die(rng->num_rngs, sizeof(uint64_t));
    rng->samples = malloc_or_die(rng->num_samples * sizeof(uint64_t));
    memory_copy(rng->samples, args->samples,
                rng->num_samples * sizeof(uint64_t));
    break;
  }
  return rng;
}

void rng_destroy(RNG *rng) {
  switch (rng->type) {
  case RNG_XOSHIRO:
    for (int i = 0; i < rng->num_rngs; i++) {
      prng_destroy(rng->xoshiro_prngs[i]);
    }
    free(rng->xoshiro_prngs);
    break;
  case RNG_FILE:
    free(rng->samples);
    free(rng->indexes);
    break;
  }
  free(rng);
}

double rng_uniform_unit(RNG *rng, int thread_index) {
  return (double)prng_next(rng->xoshiro_prngs[thread_index]) /
         ((double)UINT64_MAX);
}

// Implements the polar form of the Box-Muller transform
double rng_normal(RNG *rng, double mean, double stddev, int thread_index) {
  double sample;
  switch (rng->type) {
  case RNG_XOSHIRO:
      // Empty statement to allow declaration
      ;
    double u, v, s;
    s = 2.0;
    while (s >= 1.0 || s == 0.0) {
      u = 2.0 * rng_uniform_unit(rng, thread_index) - 1.0;
      v = 2.0 * rng_uniform_unit(rng, thread_index) - 1.0;
      s = u * u + v * v;
    }
    s = sqrt(-2.0 * log(s) / s);
    sample = mean + stddev * u * s;
    break;
  case RNG_FILE:
    if (rng->indexes[thread_index] >= rng->num_samples) {
      log_fatal("RNG file for rng %d has run out of samples", thread_index);
    }
    sample = mean + stddev * rng->samples[rng->indexes[thread_index]++];
    break;
  }
  return sample;
}