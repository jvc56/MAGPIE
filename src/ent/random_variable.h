#ifndef RANDOM_VARIABLE_H
#define RANDOM_VARIABLE_H

#include <stdint.h>

typedef struct RandomVariables RandomVariables;

typedef enum {
  RANDOM_VARIABLES_NORMAL,
  RANDOM_VARIABLES_SIMMED_PLAYS,
  RANDOM_VARIABLES_PRECOMPUTED
} random_variables_t;

typedef struct RVSubArgsNormal {
  double *means_and_stdevs;
  uint64_t seed;
} RVSubArgsNormal;

typedef struct RVSubArgsPrecomputed {
  uint64_t num_samples;
  double *samples;
} RVSubArgsPrecomputed;

typedef struct RandomVariablesArgs {
  random_variables_t type;
  uint64_t num_rvs;
  union {
    RVSubArgsNormal normal;
    RVSubArgsPrecomputed precomputed;
  };
} RandomVariablesArgs;

RandomVariables *rvs_create(RandomVariablesArgs *rvs_args);
void rvs_destroy(RandomVariables *rvs);
double rvs_sample(RandomVariables *rvs, uint64_t k);
int rvs_get_num_rvs(RandomVariables *rvs);

#endif