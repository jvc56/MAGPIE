#include <math.h>
#include <stdlib.h>

#include "random_variable.h"
#include "xoshiro.h"

#include "../util/log.h"
#include "../util/string_util.h"
#include "../util/util.h"

typedef double (*rvs_sample_func_t)(RandomVariables *, uint64_t);
typedef void (*rvs_destroy_data_func_t)(RandomVariables *);

struct RandomVariables {
  int num_rvs;
  rvs_sample_func_t sample_func;
  rvs_destroy_data_func_t destroy_data_func;
  void *data;
};

typedef struct RVNormal {
  XoshiroPRNG **xoshiro_prngs;
  double *means_and_stdevs;
} RVNormal;

double uniform_sample(XoshiroPRNG *prng) {
  return (double)prng_next(prng) / ((double)UINT64_MAX);
}

double rv_normal_sample(RandomVariables *rvs, uint64_t k) {
  RVNormal *rv_normal = (RVNormal *)rvs->data;
  double u, v, s;
  s = 2.0;
  while (s >= 1.0 || s == 0.0) {
    u = 2.0 * uniform_sample(rv_normal->xoshiro_prngs[k]) - 1.0;
    v = 2.0 * uniform_sample(rv_normal->xoshiro_prngs[k]) - 1.0;
    s = u * u + v * v;
  }
  s = sqrt(-2.0 * log(s) / s);
  return rv_normal->means_and_stdevs[k] +
         rv_normal->means_and_stdevs[k + 1] * u * s;
}

void rv_normal_destroy(RandomVariables *rvs) {
  RVNormal *rv_normal = (RVNormal *)rvs->data;
  for (int i = 0; i < rvs->num_rvs; i++) {
    prng_destroy(rv_normal->xoshiro_prngs[i]);
  }
  free(rv_normal->xoshiro_prngs);
  free(rv_normal->means_and_stdevs);
  free(rv_normal);
}

void rv_normal_create(RandomVariables *rvs, RVSubArgsNormal *args) {
  // Set the sample function
  rvs->sample_func = rv_normal_sample;

  // Set the destroy function
  rvs->destroy_data_func = rv_normal_destroy;

  // Set the data
  RVNormal *rv_normal = malloc_or_die(sizeof(RVNormal));
  rv_normal->xoshiro_prngs =
      malloc_or_die(rvs->num_rvs * sizeof(XoshiroPRNG *));
  for (int i = 0; i < rvs->num_rvs; i++) {
    rv_normal->xoshiro_prngs[i] = prng_create(args->seed);
  }
  rv_normal->means_and_stdevs =
      malloc_or_die(rvs->num_rvs * 2 * sizeof(double));
  memory_copy(rv_normal->means_and_stdevs, args->means_and_stdevs,
              rvs->num_rvs * 2 * sizeof(double));
  rvs->data = rv_normal;
}

typedef struct RVPrecomputed {
  uint64_t num_samples;
  uint64_t *indexes;
  double *samples;
} RVPrecomputed;

double rv_precomputed_sample(RandomVariables *rvs, uint64_t k) {
  RVPrecomputed *rv_precomputed = (RVPrecomputed *)rvs->data;
  if (k >= rv_precomputed->num_samples) {
    log_fatal("ran out of precomputed samples for random variable %d\n", k);
  }
  return rv_precomputed->samples[rv_precomputed->indexes[k]++];
}

void rv_precomputed_destroy(RandomVariables *rvs) {
  RVPrecomputed *rv_precomputed = (RVPrecomputed *)rvs->data;
  free(rv_precomputed->indexes);
  free(rv_precomputed->samples);
  free(rv_precomputed);
}

void rv_precomputed_create(RandomVariables *rvs, RVSubArgsPrecomputed *args) {
  // Set the sample function
  rvs->sample_func = rv_precomputed_sample;

  // Set the destroy function
  rvs->destroy_data_func = rv_precomputed_destroy;

  // Set the data
  RVPrecomputed *rv_precomputed = malloc_or_die(sizeof(RVPrecomputed));
  rv_precomputed->num_samples = args->num_samples;
  rv_precomputed->indexes = calloc_or_die(rvs->num_rvs, sizeof(uint64_t));
  rv_precomputed->samples =
      malloc_or_die(rv_precomputed->num_samples * sizeof(double));
  memory_copy(rv_precomputed->samples, args->samples,
              rv_precomputed->num_samples * sizeof(double));
  rvs->data = rv_precomputed;
}

RandomVariables *rvs_create(RandomVariablesArgs *rvs_args) {
  RandomVariables *rvs = malloc_or_die(sizeof(RandomVariables));
  rvs->num_rvs = rvs_args->num_rvs;
  switch (rvs_args->type) {
  case RANDOM_VARIABLES_NORMAL:
    rv_normal_create(rvs, &(rvs_args->normal));
    break;
  case RANDOM_VARIABLES_SIMMED_PLAYS:
    log_fatal("rvs simmed plays not implemented\n");
    break;
  case RANDOM_VARIABLES_PRECOMPUTED:
    rv_precomputed_create(rvs, &(rvs_args->precomputed));
    break;
  }
  return rvs;
}

void rvs_destroy(RandomVariables *rvs) {
  rvs->destroy_data_func(rvs);
  free(rvs);
}

double rvs_sample(RandomVariables *rvs, uint64_t k) {
  return rvs->sample_func(rvs, k);
}

int rvs_get_num_rvs(RandomVariables *rvs) { return rvs->num_rvs; }