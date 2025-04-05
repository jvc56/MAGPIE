#include <math.h>
#include <stdlib.h>

#include "bai_logger.h"
#include "random_variable.h"
#include "xoshiro.h"

#include "../util/log.h"
#include "../util/string_util.h"
#include "../util/util.h"

#define SIMILARITY_EPSILON 1e-6

typedef double (*rvs_sample_func_t)(RandomVariables *, const uint64_t,
                                    BAILogger *);
typedef bool (*rvs_similar_func_t)(RandomVariables *, const int, const int);
typedef bool (*rvs_is_epigon_func_t)(RandomVariables *, const int);
typedef void (*rvs_destroy_data_func_t)(RandomVariables *);

struct RandomVariables {
  int num_rvs;
  rvs_sample_func_t sample_func;
  rvs_similar_func_t similar_func;
  rvs_is_epigon_func_t is_epigon_func;
  rvs_destroy_data_func_t destroy_data_func;
  void *data;
};

double uniform_sample(XoshiroPRNG *prng) {
  return (double)prng_next(prng) / ((double)UINT64_MAX);
}

typedef struct RVUniform {
  XoshiroPRNG *xoshiro_prng;
} RVUniform;

double rv_uniform_sample(RandomVariables *rvs,
                         const uint64_t __attribute__((unused)) k,
                         BAILogger __attribute__((unused)) * bai_logger) {
  RVUniform *rv_uniform = (RVUniform *)rvs->data;
  return uniform_sample(rv_uniform->xoshiro_prng);
}

bool rv_uniform_mark_as_epigon_if_similar(RandomVariables
                                              __attribute__((unused)) *
                                              rvs,
                                          const int __attribute__((unused)) i,
                                          const int __attribute__((unused)) j) {
  return false;
}

bool rv_uniform_is_epigon(RandomVariables __attribute__((unused)) * rvs,
                          const int __attribute__((unused)) i) {
  return false;
}

void rv_uniform_destroy(RandomVariables *rvs) {
  RVUniform *rv_uniform = (RVUniform *)rvs->data;
  prng_destroy(rv_uniform->xoshiro_prng);
  free(rv_uniform);
}

void rv_uniform_create(RandomVariables *rvs, const uint64_t seed) {
  rvs->sample_func = rv_uniform_sample;
  rvs->similar_func = rv_uniform_mark_as_epigon_if_similar;
  rvs->is_epigon_func = rv_uniform_is_epigon;
  rvs->destroy_data_func = rv_uniform_destroy;
  RVUniform *rv_uniform = malloc_or_die(sizeof(RVUniform));
  rv_uniform->xoshiro_prng = prng_create(seed);
  rvs->data = rv_uniform;
}

typedef struct RVUniformPredetermined {
  uint64_t num_samples;
  uint64_t index;
  double *samples;
} RVUniformPredetermined;

double rv_uniform_predetermined_sample(RandomVariables *rvs,
                                       const uint64_t __attribute__((unused)) k,
                                       BAILogger __attribute__((unused)) *
                                           bai_logger) {
  RVUniformPredetermined *rv_uniform_predetermined =
      (RVUniformPredetermined *)rvs->data;
  if (rv_uniform_predetermined->index >=
      rv_uniform_predetermined->num_samples) {
    log_fatal("ran out of uniform predetermined samples\n");
  }
  const int index = rv_uniform_predetermined->index++;
  const double result = rv_uniform_predetermined->samples[index];
  return result;
}

bool rv_uniform_predetermined_mark_as_epigon_if_similar(
    RandomVariables __attribute__((unused)) * rvs,
    const int __attribute__((unused)) i, const int __attribute__((unused)) j) {
  return false;
}

bool rv_uniform_predetermined_is_epigon(RandomVariables
                                            __attribute__((unused)) *
                                            rvs,
                                        const int __attribute__((unused)) i) {
  return false;
}

void rv_uniform_predetermined_destroy(RandomVariables *rvs) {
  RVUniformPredetermined *rv_uniform_predetermined =
      (RVUniformPredetermined *)rvs->data;
  free(rv_uniform_predetermined->samples);
  free(rv_uniform_predetermined);
}

void rv_uniform_predetermined_create(RandomVariables *rvs,
                                     const double *samples,
                                     const uint64_t num_samples) {
  rvs->sample_func = rv_uniform_predetermined_sample;
  rvs->similar_func = rv_uniform_predetermined_mark_as_epigon_if_similar;
  rvs->is_epigon_func = rv_uniform_predetermined_is_epigon;
  rvs->destroy_data_func = rv_uniform_predetermined_destroy;
  RVUniformPredetermined *rv_uniform_predetermined =
      malloc_or_die(sizeof(RVUniformPredetermined));
  rv_uniform_predetermined->num_samples = num_samples;
  rv_uniform_predetermined->index = 0;
  rv_uniform_predetermined->samples =
      malloc_or_die(rv_uniform_predetermined->num_samples * sizeof(double));
  memory_copy(rv_uniform_predetermined->samples, samples,
              rv_uniform_predetermined->num_samples * sizeof(double));
  rvs->data = rv_uniform_predetermined;
}

typedef struct RVNormal {
  XoshiroPRNG *xoshiro_prng;
  double *means_and_vars;
  bool *is_epigon;
} RVNormal;

double rv_normal_sample(RandomVariables *rvs, const uint64_t k,
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
  return rv_normal->means_and_vars[k * 2] +
         rv_normal->means_and_vars[k * 2 + 1] * u * s;
}

bool rv_normal_mark_as_epigon_if_similar(RandomVariables *rvs, const int leader,
                                         const int i) {
  if (leader == i) {
    return false;
  }
  RVNormal *rv_normal = (RVNormal *)rvs->data;
  rv_normal->is_epigon[i] =
      fabs(rv_normal->means_and_vars[leader * 2] -
           rv_normal->means_and_vars[i * 2]) < SIMILARITY_EPSILON &&
      fabs(rv_normal->means_and_vars[leader * 2 + 1] -
           rv_normal->means_and_vars[i * 2 + 1]) < SIMILARITY_EPSILON;
  return rv_normal->is_epigon[i];
}

bool rv_normal_is_epigon(RandomVariables *rvs, const int i) {
  RVNormal *rv_normal = (RVNormal *)rvs->data;
  return rv_normal->is_epigon[i];
}

void rv_normal_destroy(RandomVariables *rvs) {
  RVNormal *rv_normal = (RVNormal *)rvs->data;
  prng_destroy(rv_normal->xoshiro_prng);
  free(rv_normal->means_and_vars);
  free(rv_normal->is_epigon);
  free(rv_normal);
}

void rv_normal_create(RandomVariables *rvs, const uint64_t seed,
                      const double *means_and_vars) {
  rvs->sample_func = rv_normal_sample;
  rvs->similar_func = rv_normal_mark_as_epigon_if_similar;
  rvs->is_epigon_func = rv_normal_is_epigon;
  rvs->destroy_data_func = rv_normal_destroy;
  RVNormal *rv_normal = malloc_or_die(sizeof(RVNormal));
  rv_normal->xoshiro_prng = prng_create(seed);
  rv_normal->means_and_vars = malloc_or_die(rvs->num_rvs * 2 * sizeof(double));
  memory_copy(rv_normal->means_and_vars, means_and_vars,
              rvs->num_rvs * 2 * sizeof(double));
  rv_normal->is_epigon = calloc_or_die(rvs->num_rvs, sizeof(bool));
  rvs->data = rv_normal;
}

typedef struct RVNormalPredetermined {
  uint64_t num_samples;
  uint64_t index;
  double *samples;
  double *means_and_vars;
  bool *is_epigon;
} RVNormalPredetermined;

double rv_normal_predetermined_sample(RandomVariables *rvs, const uint64_t k,
                                      BAILogger *bai_logger) {
  RVNormalPredetermined *rv_normal_predetermined =
      (RVNormalPredetermined *)rvs->data;
  if (rv_normal_predetermined->index >= rv_normal_predetermined->num_samples) {
    log_fatal("ran out of normal predetermined samples\n");
  }
  const double mean = rv_normal_predetermined->means_and_vars[k * 2];
  const double sigma2 = rv_normal_predetermined->means_and_vars[k * 2 + 1];
  const int index = rv_normal_predetermined->index++;
  const double sample = rv_normal_predetermined->samples[index];
  const double result = mean + sqrt(sigma2) * sample;
  bai_logger_log_title(bai_logger, "DETERMINISTIC_SAMPLE");
  bai_logger_log_int(bai_logger, "index", index + 1);
  bai_logger_log_int(bai_logger, "arm", k + 1);
  bai_logger_log_double(bai_logger, "s", result);
  bai_logger_log_double(bai_logger, "u", mean);
  bai_logger_log_double(bai_logger, "sigma2", sigma2);
  bai_logger_log_double(bai_logger, "samp", sample);
  bai_logger_flush(bai_logger);
  return result;
}

bool rv_normal_predetermined_mark_as_epigon_if_similar(RandomVariables *rvs,
                                                       const int leader,
                                                       const int i) {
  if (leader == i) {
    return false;
  }
  RVNormalPredetermined *rv_normal_predetermined =
      (RVNormalPredetermined *)rvs->data;
  rv_normal_predetermined->is_epigon[i] =
      fabs(rv_normal_predetermined->means_and_vars[leader * 2] -
           rv_normal_predetermined->means_and_vars[i * 2]) <
          SIMILARITY_EPSILON &&
      fabs(rv_normal_predetermined->means_and_vars[leader * 2 + 1] -
           rv_normal_predetermined->means_and_vars[i * 2 + 1]) <
          SIMILARITY_EPSILON;
  return rv_normal_predetermined->is_epigon[leader];
}

bool rv_normal_predetermined_is_epigon(RandomVariables *rvs, const int i) {
  RVNormalPredetermined *rv_normal_predetermined =
      (RVNormalPredetermined *)rvs->data;
  return rv_normal_predetermined->is_epigon[i];
}

void rv_normal_predetermined_destroy(RandomVariables *rvs) {
  RVNormalPredetermined *rv_normal_predetermined =
      (RVNormalPredetermined *)rvs->data;
  free(rv_normal_predetermined->samples);
  free(rv_normal_predetermined->means_and_vars);
  free(rv_normal_predetermined->is_epigon);
  free(rv_normal_predetermined);
}

void rv_normal_predetermined_create(RandomVariables *rvs, const double *samples,
                                    const uint64_t num_samples,
                                    const double *means_and_vars) {
  rvs->sample_func = rv_normal_predetermined_sample;
  rvs->similar_func = rv_normal_predetermined_mark_as_epigon_if_similar;
  rvs->is_epigon_func = rv_normal_predetermined_is_epigon;
  rvs->destroy_data_func = rv_normal_predetermined_destroy;
  RVNormalPredetermined *rv_normal_predetermined =
      malloc_or_die(sizeof(RVNormalPredetermined));
  rv_normal_predetermined->num_samples = num_samples;
  rv_normal_predetermined->index = 0;
  rv_normal_predetermined->samples =
      malloc_or_die(rv_normal_predetermined->num_samples * sizeof(double));
  memory_copy(rv_normal_predetermined->samples, samples,
              rv_normal_predetermined->num_samples * sizeof(double));
  rv_normal_predetermined->means_and_vars =
      malloc_or_die(rvs->num_rvs * 2 * sizeof(double));
  memory_copy(rv_normal_predetermined->means_and_vars, means_and_vars,
              rvs->num_rvs * 2 * sizeof(double));
  rv_normal_predetermined->is_epigon =
      calloc_or_die(rvs->num_rvs, sizeof(bool));
  rvs->data = rv_normal_predetermined;
}

RandomVariables *rvs_create(RandomVariablesArgs *rvs_args) {
  RandomVariables *rvs = malloc_or_die(sizeof(RandomVariables));
  rvs->num_rvs = rvs_args->num_rvs;
  switch (rvs_args->type) {
  case RANDOM_VARIABLES_UNIFORM:
    rv_uniform_create(rvs, rvs_args->seed);
    break;
  case RANDOM_VARIABLES_UNIFORM_PREDETERMINED:
    rv_uniform_predetermined_create(rvs, rvs_args->samples,
                                    rvs_args->num_samples);
    break;
  case RANDOM_VARIABLES_NORMAL:
    rv_normal_create(rvs, rvs_args->seed, rvs_args->means_and_vars);
    break;
  case RANDOM_VARIABLES_NORMAL_PREDETERMINED:
    rv_normal_predetermined_create(rvs, rvs_args->samples,
                                   rvs_args->num_samples,
                                   rvs_args->means_and_vars);
    break;
  case RANDOM_VARIABLES_SIMMED_PLAYS:
    log_fatal("rvs simmed plays not implemented\n");
    break;
  }
  return rvs;
}

void rvs_destroy(RandomVariables *rvs) {
  rvs->destroy_data_func(rvs);
  free(rvs);
}

double rvs_sample(RandomVariables *rvs, uint64_t k, BAILogger *bai_logger) {
  return rvs->sample_func(rvs, k, bai_logger);
}

// Returns the similarity between the leader and the i-th arm and marks
// the arm accordingly.
bool rvs_mark_as_epigon_if_similar(RandomVariables *rvs, int leader, int i) {
  return rvs->similar_func(rvs, leader, i);
}

bool rvs_is_epigon(RandomVariables *rvs, int i) {
  return rvs->is_epigon_func(rvs, i);
}

int rvs_get_num_rvs(RandomVariables *rvs) { return rvs->num_rvs; }