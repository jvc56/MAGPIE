#include <stdlib.h>

#include "random_variable.h"
#include "rng.h"

#include "../util/log.h"
#include "../util/string_util.h"
#include "../util/util.h"

typedef double (*rv_sample_func_t)(RandomVariables *, int);

struct RandomVariables {
  random_variables_t type;
  void *data;
  int num_rvs;
  RNG *rng;
  rv_sample_func_t sample_func;
};

double normal_sample(RandomVariables *rvs, int k) {
  double *means_and_stdevs = (double *)rvs->data;
  return rng_normal(rvs->rng, means_and_stdevs[k], means_and_stdevs[k + 1], k);
}

RandomVariables *rvs_create(RandomVariablesArgs *rvs_args) {
  RandomVariables *rvs = malloc_or_die(sizeof(RandomVariables));
  rvs->num_rvs = rvs_args->num_rvs;
  switch (rvs_args->type) {
  case RANDOM_VARIABLES_NORMAL:
    rvs->rng = rng_create(rvs_args->rng_args);
    rvs->sample_func = normal_sample;
    rvs->data = malloc_or_die(rvs->num_rvs * 2 * sizeof(double));
    memory_copy(rvs->data, rvs_args->means_and_stdevs,
                rvs_args->num_rvs * 2 * sizeof(double));
    break;
  case RANDOM_VARIABLES_SIMMED_PLAYS:
    log_fatal("rvs simmed plays not implemented\n");
    break;
  }
  return rvs;
}

void rvs_destroy(RandomVariables *rvs) {
  switch (rvs->type) {
  case RANDOM_VARIABLES_NORMAL:
    rng_destroy(rvs->rng);
    free(rvs->data);
    break;
  case RANDOM_VARIABLES_SIMMED_PLAYS:
    log_fatal("rvs simmed plays not implemented\n");
    break;
  }
}

double rvs_sample(RandomVariables *rvs, int k) {
  return rvs->sample_func(rvs, k);
}

int rvs_get_num_rvs(RandomVariables *rvs) { return rvs->num_rvs; }