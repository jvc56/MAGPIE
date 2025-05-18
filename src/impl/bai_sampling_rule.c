/*
 * Implements algorithms described in
 *
 * Dealing with Unknown Variances in Best-Arm Identification
 * (https://arxiv.org/pdf/2210.00974)
 *
 * with Julia source code kindly provided by Marc Jourdan.
 */
#include "bai_sampling_rule.h"

#include <stdbool.h>
#include <stdio.h>

#include "../def/bai_defs.h"

#include "bai_logger.h"
#include "bai_peps.h"
#include "bai_tracking.h"
#include "random_variable.h"

#include "../util/io.h"
#include "../util/util.h"

int round_robin_next_sample(const void __attribute__((unused)) * data,
                            const int __attribute__((unused)) astar,
                            const int __attribute__((unused)) aalt,
                            const double __attribute__((unused)) * ξ,
                            const double __attribute__((unused)) * ϕ2,
                            const int *N,
                            const double __attribute__((unused)) * S,
                            const double __attribute__((unused)) * Zs,
                            const int size,
                            RandomVariables __attribute__((unused)) * rng,
                            BAILogger *bai_logger) {
  int sample = 0;
  int min = N[0];
  for (int i = 1; i < size; i++) {
    if (N[i] < min) {
      min = N[i];
      sample = i;
    }
  }
  bai_logger_log_title(bai_logger, "ROUND_ROBIN");
  bai_logger_log_int(bai_logger, "sample", sample + 1);
  return sample;
}

typedef struct TrackAndStop {
  BAITracking *tracking_rule;
  BAIOracleResult *oracle_result;
} TrackAndStop;

void *track_and_stop_create(bai_tracking_t tracking_type, const int *N,
                            const int size) {
  TrackAndStop *track_and_stop = malloc_or_die(sizeof(TrackAndStop));
  track_and_stop->tracking_rule = bai_tracking_create(tracking_type, N, size);
  track_and_stop->oracle_result = bai_oracle_result_create(size);
  return track_and_stop;
}

void track_and_stop_destroy(TrackAndStop *track_and_stop) {
  bai_tracking_destroy(track_and_stop->tracking_rule);
  bai_oracle_result_destroy(track_and_stop->oracle_result);
  free(track_and_stop);
}

int track_and_stop_next_sample(
    const void *data, const int __attribute__((unused)) astar,
    const int __attribute__((unused)) aalt, const double *ξ, const double *ϕ2,
    const int *N, const double __attribute__((unused)) * S,
    const double __attribute__((unused)) * Zs, const int size,
    RandomVariables __attribute__((unused)) * rng, BAILogger *bai_logger) {
  TrackAndStop *track_and_stop = (TrackAndStop *)data;
  bai_oracle(ξ, ϕ2, size, track_and_stop->oracle_result, bai_logger);
  int sample = bai_tracking_track(track_and_stop->tracking_rule, N,
                                  track_and_stop->oracle_result->ws_over_Σ,
                                  size, bai_logger);
  return sample;
}

void track_and_stop_swap_indexes(void *data, const int i, const int j,
                                 BAILogger *bai_logger) {
  TrackAndStop *track_and_stop = (TrackAndStop *)data;
  bai_tracking_swap_indexes(track_and_stop->tracking_rule, i, j, bai_logger);
}

typedef enum {
  BAI_TOP_TWO_CHALLENGER_TC,
  BAI_TOP_TWO_CHALLENGER_TCI,
} bai_top_two_challenger_t;

typedef struct TopTwo {
  double β;
  bai_top_two_challenger_t challenger;
} TopTwo;

void *top_two_create(const double β, bai_top_two_challenger_t challenger) {
  TopTwo *top_two = malloc_or_die(sizeof(TopTwo));
  top_two->β = β;
  top_two->challenger = challenger;
  return top_two;
}

void top_two_destroy(TopTwo *top_two) { free(top_two); }

int top_two_next_sample(const void *data,
                        const int __attribute__((unused)) astar,
                        const int __attribute__((unused)) aalt,
                        const double __attribute__((unused)) * ξ,
                        const double __attribute__((unused)) * ϕ2, const int *N,
                        const double __attribute__((unused)) * S,
                        const double *Zs, const int size, RandomVariables *rng,
                        BAILogger *bai_logger) {
  TopTwo *top_two = (TopTwo *)data;
  // In this case, rng is just a simple uniform random variable between 0 and 1
  // and does not need an arm index or thread index.
  const double u = rvs_sample(rng, 0, 0, bai_logger);
  int k = 0;
  bai_logger_log_title(bai_logger, "TOP_TWO");
  bai_logger_log_int(bai_logger, "astar", astar + 1);
  if (u <= top_two->β) {
    bai_logger_log_title(bai_logger, "k = astar");
    k = astar;
  } else {
    switch (top_two->challenger) {
    case BAI_TOP_TWO_CHALLENGER_TC:
      bai_logger_log_title(bai_logger, "TC");
      for (int i = 1; i < size; i++) {
        if (Zs[i] < Zs[k]) {
          k = i;
        }
      }
      break;
    case BAI_TOP_TWO_CHALLENGER_TCI:
      bai_logger_log_title(bai_logger, "TCI");
      double k_val = Zs[0] + log((double)N[0]);
      for (int i = 1; i < size; i++) {
        const double val = Zs[i] + log((double)N[i]);
        if (val < k_val) {
          k = i;
          k_val = val;
        }
      }
      break;
    }
  }
  bai_logger_log_double(bai_logger, "u", u);
  bai_logger_log_int(bai_logger, "k", k + 1);
  bai_logger_flush(bai_logger);
  return k;
}
typedef int (*next_sample_func_t)(const void *, const int, const int,
                                  const double *, const double *, const int *,
                                  const double *, const double *, const int,
                                  RandomVariables *, BAILogger *);

typedef void (*swap_indexes_func_t)(void *, const int, const int, BAILogger *);

void bai_sampling_swap_indexes_noop(void __attribute__((unused)) * data,
                                    const int __attribute__((unused)) i,
                                    const int __attribute__((unused)) j,
                                    BAILogger __attribute__((unused)) *
                                        bai_logger) {}

struct BAISamplingRule {
  bai_sampling_rule_t type;
  void *data;
  next_sample_func_t next_sample_func;
  swap_indexes_func_t swap_indexes_func;
};

BAISamplingRule *bai_sampling_rule_create(const bai_sampling_rule_t type,
                                          const int *N, const int size) {
  BAISamplingRule *bai_sampling_rule = malloc_or_die(sizeof(BAISamplingRule));
  bai_sampling_rule->type = type;
  switch (type) {
  case BAI_SAMPLING_RULE_ROUND_ROBIN:
    bai_sampling_rule->next_sample_func = round_robin_next_sample;
    bai_sampling_rule->swap_indexes_func = bai_sampling_swap_indexes_noop;
    break;
  case BAI_SAMPLING_RULE_TRACK_AND_STOP:
    bai_sampling_rule->data = track_and_stop_create(BAI_CTRACKING, N, size);
    bai_sampling_rule->next_sample_func = track_and_stop_next_sample;
    bai_sampling_rule->swap_indexes_func = track_and_stop_swap_indexes;
    break;
  case BAI_SAMPLING_RULE_TOP_TWO:
    bai_sampling_rule->data = top_two_create(0.5, BAI_TOP_TWO_CHALLENGER_TCI);
    bai_sampling_rule->next_sample_func = top_two_next_sample;
    bai_sampling_rule->swap_indexes_func = bai_sampling_swap_indexes_noop;
    break;
  }
  return bai_sampling_rule;
}

void bai_sampling_rule_destroy(BAISamplingRule *bai_sampling_rule) {
  switch (bai_sampling_rule->type) {
  case BAI_SAMPLING_RULE_ROUND_ROBIN:
    break;
  case BAI_SAMPLING_RULE_TRACK_AND_STOP:
    track_and_stop_destroy((TrackAndStop *)bai_sampling_rule->data);
    break;
  case BAI_SAMPLING_RULE_TOP_TWO:
    top_two_destroy((TopTwo *)bai_sampling_rule->data);
    break;
  }
  free(bai_sampling_rule);
}

int bai_sampling_rule_next_sample(const BAISamplingRule *bai_sampling_rule,
                                  const int astar, const int aalt,
                                  const double *ξ, const double *ϕ2,
                                  const int *N, const double *S,
                                  const double *Zs, const int size,
                                  RandomVariables *rng, BAILogger *bai_logger) {
  return bai_sampling_rule->next_sample_func(bai_sampling_rule->data, astar,
                                             aalt, ξ, ϕ2, N, S, Zs, size, rng,
                                             bai_logger);
}

void bai_sampling_rule_swap_indexes(BAISamplingRule *bai_sampling_rule,
                                    const int i, const int j,
                                    BAILogger *bai_logger) {
  bai_sampling_rule->swap_indexes_func(bai_sampling_rule->data, i, j,
                                       bai_logger);
}