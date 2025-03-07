#include "rng.h" // Include the xoshiro PRNG header

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "xoshiro.h"

#include "../util/log.h"
#include "../util/util.h"

typedef enum { RNG_XOSHIRO, RNG_FILE } rng_t;

struct RNG {
  rng_t type;
  int num_rngs;
  XoshiroPRNG **xoshiro_prngs;
  uint64_t num_values;
  uint64_t *indexes;
  uint64_t *values;
};

RNG *rng_create(RNGArgs *args) {
  RNG *rng = malloc_or_die(sizeof(RNG));
  rng->num_rngs = args->num_rngs;
  rng->type = RNG_XOSHIRO;
  if (args->filename) {
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
      // Empty statement to allow declaration
      ;
    FILE *file = fopen(args->filename, "r");
    if (!file) {
      log_fatal("Failed to open RNG file: %s", args->filename);
    }

    rng->num_values = 0;
    uint64_t value;
    while (fscanf(file, "%lu\n", &value) == 1) {
      rng->num_values++;
    }

    if (ferror(file)) {
      log_fatal("Error reading RNG file: %s", args->filename);
    }

    rng->values = malloc_or_die(rng->num_values * sizeof(uint64_t));
    rng->indexes = calloc_or_die(rng->num_values, sizeof(uint64_t));
    rewind(file);
    for (uint64_t i = 0; i < rng->num_values; i++) {
      if (fscanf(file, "%lu\n", &rng->values[i]) != 1) {
        log_fatal("Unexpected format in RNG file: %s", args->filename);
      }
    }
    fclose(file);
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
    free(rng->values);
    free(rng->indexes);
    break;
  }
  free(rng);
}

uint64_t rng_uniform_uint64(RNG *rng, int thread_index) {
  uint64_t result;
  switch (rng->type) {
  case RNG_XOSHIRO:
    result = prng_next(rng->xoshiro_prngs[thread_index]);
    break;
  case RNG_FILE:
    if (rng->indexes[thread_index] >= rng->num_values) {
      log_fatal("RNG file for rng %d has run out of values", thread_index);
    }
    result = rng->values[rng->indexes[thread_index]++];
    break;
  }
  return result;
}

double rng_uniform_unit(RNG *rng, int thread_index) {
  return (double)rng_uniform_uint64(rng, thread_index) / ((double)UINT64_MAX);
}

// Implements the polar form of the Box-Muller transform
double rng_normal(RNG *rng, double mean, double stddev, int thread_index) {
  double u, v, s;
  s = 2.0;
  while (s >= 1.0 || s == 0.0) {
    u = 2.0 * rng_uniform_unit(rng, thread_index) - 1.0;
    v = 2.0 * rng_uniform_unit(rng, thread_index) - 1.0;
    s = u * u + v * v;
  }
  s = sqrt(-2.0 * log(s) / s);
  return mean + stddev * u * s;
}