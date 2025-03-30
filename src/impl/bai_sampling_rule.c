#include "bai_sampling_rule.h"

#include <stdbool.h>
#include <stdio.h>

#include "../ent/bai_logger.h"
#include "../ent/random_variable.h"

#include "bai_peps.h"
#include "bai_tracking.h"

#include "../util/log.h"
#include "../util/util.h"

int round_robin_next_sample(void __attribute__((unused)) * data,
                            int __attribute__((unused)) astar,
                            int __attribute__((unused)) aalt,
                            double __attribute__((unused)) * ξ,
                            double __attribute__((unused)) * ϕ2, int *N,
                            double __attribute__((unused)) * S,
                            double __attribute__((unused)) * Zs, int size,
                            RandomVariables __attribute__((unused)) * rng,
                            BAILogger *bai_logger) {
  int sum = 0;
  for (int i = 0; i < size; i++) {
    sum += N[i];
  }
  const int sample = sum % size;
  bai_logger_log_title(bai_logger, "ROUND_ROBIN");
  bai_logger_log_int(bai_logger, "sample", sample + 1);
  return sample;
}

typedef struct TrackAndStop {
  bool is_EV;
  BAITracking *tracking_rule;
  BAIOracleResult *oracle_result;
} TrackAndStop;

void *track_and_stop_create(bool is_EV, bai_tracking_t tracking_type, int *N,
                            int size) {
  TrackAndStop *track_and_stop = malloc_or_die(sizeof(TrackAndStop));
  track_and_stop->is_EV = is_EV;
  track_and_stop->tracking_rule = bai_tracking_create(tracking_type, N, size);
  track_and_stop->oracle_result = bai_oracle_result_create(size);
  return track_and_stop;
}

void track_and_stop_destroy(TrackAndStop *track_and_stop) {
  bai_tracking_destroy(track_and_stop->tracking_rule);
  bai_oracle_result_destroy(track_and_stop->oracle_result);
  free(track_and_stop);
}

int track_and_stop_next_sample(void *data, int __attribute__((unused)) astar,
                               int __attribute__((unused)) aalt, double *ξ,
                               double *ϕ2, int *N,
                               double __attribute__((unused)) * S,
                               double __attribute__((unused)) * Zs, int size,
                               RandomVariables __attribute__((unused)) * rng,
                               BAILogger *bai_logger) {
  TrackAndStop *track_and_stop = (TrackAndStop *)data;
  bai_oracle(ξ, ϕ2, size, track_and_stop->is_EV, track_and_stop->oracle_result,
             bai_logger);
  int sample =
      bai_track(track_and_stop->tracking_rule, N,
                track_and_stop->oracle_result->ws_over_Σ, size, bai_logger);
  return sample;
}

typedef enum {
  BAI_TOP_TWO_CHALLENGER_TC,
  BAI_TOP_TWO_CHALLENGER_TCI,
} bai_top_two_challenger_t;

typedef struct TopTwo {
  bool is_EV;
  double β;
  bai_top_two_challenger_t challenger;
} TopTwo;

void *top_two_create(bool is_EV, double β,
                     bai_top_two_challenger_t challenger) {
  TopTwo *top_two = malloc_or_die(sizeof(TopTwo));
  top_two->is_EV = is_EV;
  top_two->β = β;
  top_two->challenger = challenger;
  return top_two;
}

void top_two_destroy(TopTwo *top_two) { free(top_two); }

int top_two_next_sample(void *data, int __attribute__((unused)) astar,
                        int __attribute__((unused)) aalt,
                        double __attribute__((unused)) * ξ,
                        double __attribute__((unused)) * ϕ2, int *N,
                        double __attribute__((unused)) * S, double *Zs,
                        int size, RandomVariables *rng, BAILogger *bai_logger) {
  TopTwo *top_two = (TopTwo *)data;
  const double u = rvs_sample(rng, 0, bai_logger);
  int k;
  bai_logger_log_title(bai_logger, "TOP_TWO");
  bai_logger_log_int(bai_logger, "astar", astar + 1);
  if (u <= top_two->β) {
    bai_logger_log_title(bai_logger, "k = astar");
    k = astar;
  } else {
    double k_val;
    switch (top_two->challenger) {
    case BAI_TOP_TWO_CHALLENGER_TC:
      bai_logger_log_title(bai_logger, "TC");
      k = 0;
      k_val = Zs[0];
      for (int i = 1; i < size; i++) {
        if (Zs[i] < k_val) {
          k = i;
          k_val = Zs[i];
        }
      }
      break;
    case BAI_TOP_TWO_CHALLENGER_TCI:
      bai_logger_log_title(bai_logger, "TCI");
      k = 0;
      k_val = Zs[0] + log((double)N[0]);
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
typedef int (*next_sample_func_t)(void *, int, int, double *, double *, int *,
                                  double *, double *, int, RandomVariables *,
                                  BAILogger *);

struct BAISamplingRule {
  bai_sampling_rule_t type;
  void *data;
  next_sample_func_t next_sample_func;
};

BAISamplingRule *bai_sampling_rule_create(bai_sampling_rule_t type, bool is_EV,
                                          int *N, int size) {
  BAISamplingRule *bai_sampling_rule = malloc_or_die(sizeof(BAISamplingRule));
  bai_sampling_rule->type = type;
  switch (type) {
  case BAI_SAMPLING_RULE_ROUND_ROBIN:
    bai_sampling_rule->next_sample_func = round_robin_next_sample;
    break;
  case BAI_SAMPLING_RULE_TRACK_AND_STOP:
    bai_sampling_rule->data =
        track_and_stop_create(is_EV, BAI_CTRACKING, N, size);
    bai_sampling_rule->next_sample_func = track_and_stop_next_sample;
    break;
  case BAI_SAMPLING_RULE_TOP_TWO:
    bai_sampling_rule->data =
        top_two_create(is_EV, 0.5, BAI_TOP_TWO_CHALLENGER_TCI);
    bai_sampling_rule->next_sample_func = top_two_next_sample;
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

int bai_sampling_rule_next_sample(BAISamplingRule *bai_sampling_rule, int astar,
                                  int aalt, double *ξ, double *ϕ2, int *N,
                                  double *S, double *Zs, int size,
                                  RandomVariables *rng, BAILogger *bai_logger) {
  return bai_sampling_rule->next_sample_func(bai_sampling_rule->data, astar,
                                             aalt, ξ, ϕ2, N, S, Zs, size, rng,
                                             bai_logger);
}