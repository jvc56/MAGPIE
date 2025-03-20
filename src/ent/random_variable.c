#include <math.h>
#include <stdlib.h>

#include "bai_logger.h"
#include "random_variable.h"
#include "xoshiro.h"

#include "../util/log.h"
#include "../util/string_util.h"
#include "../util/util.h"

typedef double (*rvs_sample_func_t)(RandomVariables *, uint64_t, BAILogger *);
typedef void (*rvs_destroy_data_func_t)(RandomVariables *);

struct RandomVariables {
  int num_rvs;
  double *means_and_stdevs;
  rvs_sample_func_t sample_func;
  rvs_destroy_data_func_t destroy_data_func;
  void *data;
};

typedef struct RVNormal {
  XoshiroPRNG *xoshiro_prng;
} RVNormal;

double uniform_sample(XoshiroPRNG *prng) {
  return (double)prng_next(prng) / ((double)UINT64_MAX);
}

double rv_normal_sample(RandomVariables *rvs, uint64_t k,
                        BAILogger __attribute__((unused)) * bai_logger) {
  // Implements the Box-Muller transform
  RVNormal *rv_normal = (RVNormal *)rvs->data;
  double u, v, s;
  s = 2.0;
  while (s >= 1.0 || s == 0.0) {
    u = 2.0 * uniform_sample(rv_normal->xoshiro_prng) - 1.0;
    v = 2.0 * uniform_sample(rv_normal->xoshiro_prng) - 1.0;
    s = u * u + v * v;
  }
  s = sqrt(-2.0 * log(s) / s);
  return rvs->means_and_stdevs[k * 2] +
         rvs->means_and_stdevs[k * 2 + 1] * u * s;
}

void rv_normal_destroy(RandomVariables *rvs) {
  RVNormal *rv_normal = (RVNormal *)rvs->data;
  prng_destroy(rv_normal->xoshiro_prng);
  free(rv_normal);
}

void rv_normal_create(RandomVariables *rvs, RVSubArgsNormal *args) {
  // Set the sample function
  rvs->sample_func = rv_normal_sample;

  // Set the destroy function
  rvs->destroy_data_func = rv_normal_destroy;

  // Set the data
  RVNormal *rv_normal = malloc_or_die(sizeof(RVNormal));
  rv_normal->xoshiro_prng = prng_create(args->seed);
  rvs->data = rv_normal;
}

typedef struct RVNormalPredetermined {
  uint64_t num_samples;
  uint64_t index;
  double *samples;
} RVNormalPredetermined;

double rv_normal_predetermined_sample(RandomVariables *rvs, uint64_t k,
                                      BAILogger *bai_logger) {
  RVNormalPredetermined *rv_normal_predetermined =
      (RVNormalPredetermined *)rvs->data;
  if (rv_normal_predetermined->index >= rv_normal_predetermined->num_samples) {
    log_fatal("ran out of normal predetermined samples\n");
  }
  const double mean = rvs->means_and_stdevs[k * 2];
  const double sigma2 = rvs->means_and_stdevs[k * 2 + 1];
  const int index = rv_normal_predetermined->index++;
  const double sample = rv_normal_predetermined->samples[index];
  const double result = mean + sqrt(sigma2) * sample;
  bai_logger_log_title(bai_logger, "DETERMINISTIC_SAMPLE");
  bai_logger_log_int(bai_logger, "index", index);
  bai_logger_log_int(bai_logger, "arm", k);
  bai_logger_log_double(bai_logger, "s", result);
  bai_logger_log_double(bai_logger, "u", mean);
  bai_logger_log_double(bai_logger, "sigma2", sigma2);
  bai_logger_log_double(bai_logger, "sampe", sample);
  bai_logger_flush(bai_logger);
  return result;
}

void rv_normal_predetermined_destroy(RandomVariables *rvs) {
  RVNormalPredetermined *rv_normal_predetermined =
      (RVNormalPredetermined *)rvs->data;
  free(rv_normal_predetermined->samples);
  free(rv_normal_predetermined);
}

void rv_normal_predetermined_create(RandomVariables *rvs,
                                    RVSubArgsNormalPredetermined *args) {
  // Set the sample function
  rvs->sample_func = rv_normal_predetermined_sample;

  // Set the destroy function
  rvs->destroy_data_func = rv_normal_predetermined_destroy;

  // Set the data
  RVNormalPredetermined *rv_normal_predetermined =
      malloc_or_die(sizeof(RVNormalPredetermined));
  rv_normal_predetermined->num_samples = args->num_samples;
  rv_normal_predetermined->index = 0;
  rv_normal_predetermined->samples =
      malloc_or_die(rv_normal_predetermined->num_samples * sizeof(double));
  memory_copy(rv_normal_predetermined->samples, args->samples,
              rv_normal_predetermined->num_samples * sizeof(double));
  rvs->data = rv_normal_predetermined;
}

RandomVariables *rvs_create(RandomVariablesArgs *rvs_args) {
  RandomVariables *rvs = malloc_or_die(sizeof(RandomVariables));
  rvs->num_rvs = rvs_args->num_rvs;
  rvs->means_and_stdevs = malloc_or_die(rvs->num_rvs * 2 * sizeof(double));
  memory_copy(rvs->means_and_stdevs, rvs_args->means_and_stdevs,
              rvs->num_rvs * 2 * sizeof(double));
  switch (rvs_args->type) {
  case RANDOM_VARIABLES_NORMAL:
    rv_normal_create(rvs, &(rvs_args->normal));
    break;
  case RANDOM_VARIABLES_SIMMED_PLAYS:
    log_fatal("rvs simmed plays not implemented\n");
    break;
  case RANDOM_VARIABLES_NORMAL_PREDETERMINED:
    rv_normal_predetermined_create(rvs, &(rvs_args->normal_predetermined));
    break;
  }
  return rvs;
}

void rvs_destroy(RandomVariables *rvs) {
  rvs->destroy_data_func(rvs);
  free(rvs->means_and_stdevs);
  free(rvs);
}

double rvs_sample(RandomVariables *rvs, uint64_t k, BAILogger *bai_logger) {
  return rvs->sample_func(rvs, k, bai_logger);
}

int rvs_get_num_rvs(RandomVariables *rvs) { return rvs->num_rvs; }