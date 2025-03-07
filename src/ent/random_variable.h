#ifndef RANDOM_VARIABLE_H
#define RANDOM_VARIABLE_H

#include "rng.h"

typedef struct RandomVariables RandomVariables;

typedef enum {
  RANDOM_VARIABLES_NORMAL,
  RANDOM_VARIABLES_SIMMED_PLAYS
} random_variables_t;

typedef struct RandomVariablesArgs {
  random_variables_t type;
  int num_rvs;
  double *means_and_stdevs;
  RNGArgs *rng_args;
} RandomVariablesArgs;

RandomVariables *rvs_create(RandomVariablesArgs *rvs_args);
void rvs_destroy(RandomVariables *rvs);
double rvs_sample(RandomVariables *rvs, int i);
int rvs_get_num_rvs(RandomVariables *rvs);

#endif