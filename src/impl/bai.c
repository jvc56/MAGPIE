#include <limits.h>
#include <stdbool.h>

#include "../def/bai_defs.h"

#include "../ent/bai_logger.h"
#include "../ent/random_variable.h"
#include "../ent/thread_control.h"

#include "../util/log.h"
#include "../util/util.h"

#include "bai_helper.h"
#include "bai_peps.h"
#include "bai_sampling_rule.h"

#define MINIMUM_VARIANCE 1e-10

// Internal BAI structs

typedef struct BAIArmIndexDatum {
  int bai_index;
  int rvs_index;
  bool is_similarity_evaluated;
} BAIArmIndexDatum;

typedef struct BAIArmData {
  int initial_K;
  int K;
  int t;
  int similar_play_cutoff;
  bool threshold_reached;
  int *N;
  double *S;
  double *S2;
  double *hμ;
  double *hσ2;
  BAISamplingRule *bai_sampling_rule;
  BAIArmIndexDatum **bai_arm_index_data;
} BAIArmData;

BAIArmData *bai_arm_data_create(const int K, const int similar_play_cutoff) {
  BAIArmData *arm_data = malloc_or_die(sizeof(BAIArmData));
  arm_data->initial_K = K;
  arm_data->K = K;
  arm_data->t = 0;
  arm_data->similar_play_cutoff = similar_play_cutoff;
  arm_data->threshold_reached = false;
  arm_data->N = calloc_or_die(K, sizeof(int));
  arm_data->S = calloc_or_die(K, sizeof(double));
  arm_data->S2 = calloc_or_die(K, sizeof(double));
  arm_data->hμ = calloc_or_die(K, sizeof(double));
  arm_data->hσ2 = calloc_or_die(K, sizeof(double));
  arm_data->bai_arm_index_data =
      malloc_or_die((sizeof(BAIArmIndexDatum *)) * (K));
  for (int i = 0; i < K; i++) {
    arm_data->bai_arm_index_data[i] = malloc_or_die(sizeof(BAIArmIndexDatum));
    arm_data->bai_arm_index_data[i]->bai_index = i;
    arm_data->bai_arm_index_data[i]->rvs_index = i;
    arm_data->bai_arm_index_data[i]->is_similarity_evaluated = false;
  }
  return arm_data;
}

void bai_arm_data_destroy(BAIArmData *arm_data) {
  free(arm_data->N);
  free(arm_data->S);
  free(arm_data->S2);
  free(arm_data->hμ);
  free(arm_data->hσ2);
  bai_sampling_rule_destroy(arm_data->bai_sampling_rule);
  for (int i = 0; i < arm_data->initial_K; i++) {
    free(arm_data->bai_arm_index_data[i]);
  }
  free(arm_data->bai_arm_index_data);
  free(arm_data);
}

// BAI to RandomVariables interface

int bai_arm_data_get_rvs_index(const BAIArmData *arm_data,
                               const RandomVariables *rvs, const int k) {
  const int rvs_index = arm_data->bai_arm_index_data[k]->rvs_index;
  if (rvs_is_epigon(rvs, rvs_index)) {
    log_fatal("bai selected an arm (%d) that was marked as an epigon (%d)", k,
              rvs_index);
  }
  return rvs_index;
}

double bai_rvs_sample(BAIArmData *arm_data, RandomVariables *rvs, const int k,
                      BAILogger *bai_logger) {
  return rvs_sample(rvs, bai_arm_data_get_rvs_index(arm_data, rvs, k),
                    bai_logger);
}

bool bai_rvs_mark_as_epigon_if_similar(BAIArmData *arm_data,
                                       RandomVariables *rvs, const int leader,
                                       const int i) {
  return rvs_mark_as_epigon_if_similar(
      rvs, bai_arm_data_get_rvs_index(arm_data, rvs, leader),
      bai_arm_data_get_rvs_index(arm_data, rvs, i));
}

bool bai_rvs_is_epigon(BAIArmData *arm_data, RandomVariables *rvs,
                       const int k) {
  return rvs_is_epigon(rvs, bai_arm_data_get_rvs_index(arm_data, rvs, k));
}

void bai_arm_data_init_sampling_rule(BAIArmData *arm_data,
                                     const bai_sampling_rule_t sr,
                                     const bool is_EV, const int K) {
  arm_data->bai_sampling_rule =
      bai_sampling_rule_create(sr, is_EV, arm_data->N, K);
}

// BAIArmData functions

void bai_arm_data_sample(BAIArmData *arm_data, RandomVariables *rvs,
                         const int k, BAILogger *bai_logger) {
  const double _X = bai_rvs_sample(arm_data, rvs, k, bai_logger);
  arm_data->S[k] += _X;
  arm_data->S2[k] += _X * _X;
  arm_data->N[k] += 1;
  arm_data->hμ[k] = arm_data->S[k] / arm_data->N[k];
  arm_data->hσ2[k] =
      arm_data->S2[k] / arm_data->N[k] - arm_data->hμ[k] * arm_data->hμ[k];
  if (arm_data->hσ2[k] < MINIMUM_VARIANCE) {
    arm_data->hσ2[k] = MINIMUM_VARIANCE;
  }
  arm_data->t++;
}

void swap_indexes_bool(bool *a, const int i, const int j) {
  const bool tmp = a[i];
  a[i] = a[j];
  a[j] = tmp;
}

void swap_indexes_int(int *a, const int i, const int j) {
  const int tmp = a[i];
  a[i] = a[j];
  a[j] = tmp;
}

void swap_indexes_double(double *a, const int i, const int j) {
  const double tmp = a[i];
  a[i] = a[j];
  a[j] = tmp;
}

void bai_arm_data_swap(BAIArmData *arm_data, const int i, const int j,
                       BAILogger *bai_logger) {
  swap_indexes_int(arm_data->N, i, j);
  swap_indexes_double(arm_data->S, i, j);
  swap_indexes_double(arm_data->S2, i, j);
  swap_indexes_double(arm_data->hμ, i, j);
  swap_indexes_double(arm_data->hσ2, i, j);
  bai_sampling_rule_swap_indexes(arm_data->bai_sampling_rule, i, j, bai_logger);
  BAIArmIndexDatum *tmp = arm_data->bai_arm_index_data[i];
  arm_data->bai_arm_index_data[i] = arm_data->bai_arm_index_data[j];
  arm_data->bai_arm_index_data[j] = tmp;
  arm_data->bai_arm_index_data[i]->bai_index = i;
  arm_data->bai_arm_index_data[j]->bai_index = j;
}

// Marks arms as epigons if they are similar to astar and
// returns the value of astar.
// If epigons are evaluated, astar will be 0
// Otherwise, astar will remained unchanged.
int bai_arm_data_potentially_mark_epigons(BAIArmData *arm_data,
                                          RandomVariables *rvs, const int astar,
                                          BAILogger *bai_logger) {
  if (arm_data->similar_play_cutoff == 0 ||
      arm_data->t < arm_data->similar_play_cutoff ||
      arm_data->bai_arm_index_data[astar]->is_similarity_evaluated) {
    return astar;
  }
  bai_logger_log_title(bai_logger, "EVAL_EPIGON");
  // Always make astar the first arm.
  bai_arm_data_swap(arm_data, astar, 0, bai_logger);
  for (int i = arm_data->K - 1; i > 0; i--) {
    bai_logger_log_int(bai_logger, "evale", i);
    if (!bai_rvs_mark_as_epigon_if_similar(arm_data, rvs, 0, i)) {
      bai_logger_log_int(bai_logger, "not_epigon", i);
      continue;
    }
    bai_logger_log_int(bai_logger, "epigon_marked", i);
    bai_logger_log_int(bai_logger, "swap", arm_data->K - 1);
    bai_arm_data_swap(arm_data, i, arm_data->K - 1, bai_logger);
    arm_data->K--;
    if (arm_data->K == 1) {
      break;
    }
  }
  arm_data->bai_arm_index_data[0]->is_similarity_evaluated = true;
  return 0;
}

// BAI helper functions

bool stopping_criterion(const int K, const double *Zs, const BAIThreshold *Sβ,
                        const int *N, const double *hμ, const double *hσ2,
                        const int astar, BAILogger *bai_logger) {
  if (!Sβ) {
    return false;
  }
  for (int a = 0; a < K; a++) {
    if (a == astar) {
      continue;
    }
    // Original Julia code is:
    // val = is_glr ? Zs[a] : MZs[a];
    // cdt = val > Sβ(N, hμ, hσ2, astar, a);
    // stop = stop && cdt;
    const double thres =
        bai_invoke_threshold(Sβ, N, K, hμ, hσ2, astar, a, bai_logger);
    const bool cdt = Zs[a] > thres;
    bai_logger_log_title(bai_logger, "STOPPING_CRITERION");
    bai_logger_log_int(bai_logger, "a", a + 1);
    bai_logger_log_double(bai_logger, "val", Zs[a]);
    bai_logger_log_double(bai_logger, "thres", thres);
    bai_logger_flush(bai_logger);
    if (!cdt) {
      return false;
    }
  }
  return true;
}

bool bai_sample_limit_reached(const BAIOptions *bai_options,
                              const BAIArmData *arm_data) {
  if (bai_options->sampling_rule != BAI_SAMPLING_RULE_ROUND_ROBIN) {
    return arm_data->t >= bai_options->sample_limit;
  }
  for (int i = 0; i < arm_data->K; i++) {
    if (arm_data->N[i] < bai_options->sample_limit) {
      return false;
    }
  }
  return true;
}

bool bai_round_robin_is_complete(const BAIArmData *arm_data) {
  const int num_arm_samples = arm_data->N[0];
  for (int i = 1; i < arm_data->K; i++) {
    if (arm_data->N[i] != num_arm_samples) {
      return false;
    }
  }
  return true;
}

void bai_set_result(const bai_exit_cond_t exit_cond, const int astar,
                    const BAIArmData *arm_data, const RandomVariables *rvs,
                    BAIResult *bai_result) {
  bai_result->exit_cond = exit_cond;
  bai_result->best_arm = bai_arm_data_get_rvs_index(arm_data, rvs, astar);
  bai_result->total_samples = arm_data->t;
}

typedef struct BAIIsFinishedArgs {
  const BAIOptions *bai_options;
  BAIArmData *arm_data;
  const RandomVariables *rvs;
  ThreadControl *thread_control;
  const double *Zs;
  const BAIThreshold *Sβ;
  BAILogger *bai_logger;
  BAIResult *bai_result;
} BAIIsFinishedArgs;

// Returns true and sets the bai_result if finished.
// Returns false otherwise.
bool bai_is_finished(BAIIsFinishedArgs *args, const bai_exit_cond_t exit_cond,
                     const int astar) {
  bool finished = false;
  switch (exit_cond) {
  case BAI_EXIT_CONDITION_NONE:
    log_fatal("invalid BAI finished exit condition: ", exit_cond);
    break;
  case BAI_EXIT_CONDITION_THRESHOLD:
    args->arm_data->threshold_reached =
        args->arm_data->threshold_reached ||
        stopping_criterion(args->arm_data->K, args->Zs, args->Sβ,
                           args->arm_data->N, args->arm_data->hμ,
                           args->arm_data->hσ2, astar, args->bai_logger);
    finished =
        args->arm_data->threshold_reached &&
        (args->bai_options->sampling_rule != BAI_SAMPLING_RULE_ROUND_ROBIN ||
         bai_round_robin_is_complete(args->arm_data));
    break;
  case BAI_EXIT_CONDITION_SAMPLE_LIMIT:
    finished = bai_sample_limit_reached(args->bai_options, args->arm_data);
    break;
  case BAI_EXIT_CONDITION_TIME_LIMIT:
    finished = args->bai_options->time_limit_seconds > 0 &&
               thread_control_get_seconds_elapsed(args->thread_control) >=
                   args->bai_options->time_limit_seconds;
    break;
  case BAI_EXIT_CONDITION_USER_INTERRUPT:
    finished = thread_control_get_is_halted(args->thread_control);
    break;
  case BAI_EXIT_CONDITION_ONE_ARM_REMAINING:
    finished = args->arm_data->K == 1;
    break;
  }
  if (finished) {
    bai_set_result(exit_cond, astar, args->arm_data, args->rvs,
                   args->bai_result);
  }
  return finished;
}

// Assumes rvs are normally distributed.
// Assumes rng is uniformly distributed between 0 and 1.
void bai(const BAIOptions *bai_options, RandomVariables *rvs,
         RandomVariables *rng, ThreadControl *thread_control,
         BAILogger *bai_logger, BAIResult *bai_result) {
  const int num_rvs = rvs_get_num_rvs(rvs);
  BAIArmData *arm_data =
      bai_arm_data_create(num_rvs, bai_options->similar_play_cutoff);

  bai_set_result(BAI_EXIT_CONDITION_NONE, 0, arm_data, rvs, bai_result);

  BAIThreshold *Sβ =
      bai_create_threshold(bai_options->threshold, bai_options->is_EV,
                           bai_options->delta, 2, 2, 1.2);
  BAIGLRTResults *glrt_results = bai_glrt_results_create(num_rvs);

  for (int k = 0; k < num_rvs; k++) {
    for (int i = 0; i < 2; i++) {
      bai_arm_data_sample(arm_data, rvs, k, bai_logger);
    }
  }

  // The sampling rule must be initialized after the initial sampling.
  bai_arm_data_init_sampling_rule(arm_data, bai_options->sampling_rule,
                                  bai_options->is_EV, num_rvs);
  int astar;
  BAIIsFinishedArgs is_finished_args = {
      .bai_options = bai_options,
      .arm_data = arm_data,
      .rvs = rvs,
      .thread_control = thread_control,
      .Zs = glrt_results->vals,
      .Sβ = Sβ,
      .bai_logger = bai_logger,
      .bai_result = bai_result,
  };
  while (!bai_is_finished(&is_finished_args, BAI_EXIT_CONDITION_TIME_LIMIT,
                          astar) &&
         !bai_is_finished(&is_finished_args, BAI_EXIT_CONDITION_USER_INTERRUPT,
                          astar) &&
         !bai_is_finished(&is_finished_args,
                          BAI_EXIT_CONDITION_ONE_ARM_REMAINING, astar)) {
    bai_logger_log_int(bai_logger, "t", arm_data->t);
    bai_glrt(arm_data->K, arm_data->N, arm_data->hμ, arm_data->hσ2,
             bai_options->is_EV, glrt_results, bai_logger);
    const double *Zs = glrt_results->vals;
    const int aalt = glrt_results->k;
    astar = glrt_results->astar;
    const double *ξ = arm_data->hμ;
    const double *ϕ2 = arm_data->hσ2;

    bai_logger_log_title(bai_logger, "GLRT_RETURN_VALUES");
    bai_logger_log_double_array(bai_logger, "Zs", Zs, arm_data->K);
    bai_logger_log_int(bai_logger, "aalt", aalt + 1);
    bai_logger_log_int(bai_logger, "astar", astar + 1);
    bai_logger_log_double_array(bai_logger, "ksi", ξ, arm_data->K);
    bai_logger_log_double_array(bai_logger, "phi2", ϕ2, arm_data->K);
    bai_logger_flush(bai_logger);

    if (bai_is_finished(&is_finished_args, BAI_EXIT_CONDITION_THRESHOLD,
                        astar)) {
      break;
    }

    const int k = bai_sampling_rule_next_sample(
        arm_data->bai_sampling_rule, astar, aalt, ξ, ϕ2, arm_data->N,
        arm_data->S, Zs, arm_data->K, rng, bai_logger);
    bai_arm_data_sample(arm_data, rvs, k, bai_logger);
    if (bai_is_finished(&is_finished_args, BAI_EXIT_CONDITION_SAMPLE_LIMIT,
                        astar)) {
      break;
    }
    if (bai_options->similar_play_cutoff > 0) {
      bai_logger_log_title(bai_logger, "FINISHED_SAMPLE");
      bai_logger_log_int(bai_logger, "similar_play_cutoff",
                         bai_options->similar_play_cutoff);
      bai_logger_log_int(bai_logger, "astar", astar + 1);
      bai_logger_log_int(bai_logger, "N_astar", arm_data->N[astar]);
      bai_logger_log_int_array(bai_logger, "N", arm_data->N, arm_data->K);
      bai_logger_log_double_array(bai_logger, "avg", arm_data->hμ, arm_data->K);
      bool *is_epigon = calloc_or_die(arm_data->K, sizeof(bool));
      for (int i = 0; i < arm_data->K; i++) {
        is_epigon[i] = bai_rvs_is_epigon(arm_data, rvs, i);
      }
      bai_logger_log_bool_array(bai_logger, "is_epigon", is_epigon,
                                arm_data->K);
      free(is_epigon);
      bai_logger_flush(bai_logger);
    }
    astar =
        bai_arm_data_potentially_mark_epigons(arm_data, rvs, astar, bai_logger);
  }
  bai_glrt_results_destroy(glrt_results);
  bai_destroy_threshold(Sβ);
  bai_arm_data_destroy(arm_data);
}