#ifndef RANDOM_VARIABLE_H
#define RANDOM_VARIABLE_H

#include <stdint.h>

#include "bai_logger.h"

typedef struct RandomVariables RandomVariables;

typedef enum {
  RANDOM_VARIABLES_UNIFORM,
  RANDOM_VARIABLES_UNIFORM_PREDETERMINED,
  RANDOM_VARIABLES_NORMAL,
  RANDOM_VARIABLES_NORMAL_PREDETERMINED,
  RANDOM_VARIABLES_SIMMED_PLAYS,
} random_variables_t;

typedef struct RandomVariablesArgs {
  random_variables_t type;
  uint64_t num_rvs;
  uint64_t seed;
  uint64_t num_samples;
  const double *samples;
  const double *means_and_vars;
} RandomVariablesArgs;

RandomVariables *rvs_create(const RandomVariablesArgs *rvs_args);
void rvs_destroy(RandomVariables *rvs);
double rvs_sample(RandomVariables *rvs, uint64_t k, BAILogger *bai_logger);
void rvs_reset(RandomVariables *rvs);
bool rvs_mark_as_epigon_if_similar(RandomVariables *rvs, int leader, int i);
bool rvs_is_epigon(const RandomVariables *rvs, int i);
int rvs_get_num_rvs(const RandomVariables *rvs);
uint64_t rvs_get_total_samples(const RandomVariables *rvs);

#endif