#include "bai_sampling_rule.h"

#include <stdbool.h>
#include <stdio.h>

#include "../ent/bai_logger.h"

#include "bai_peps.h"
#include "bai_tracking.h"

#include "../util/log.h"
#include "../util/util.h"

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
                               BAILogger *bai_logger) {
  TrackAndStop *track_and_stop = (TrackAndStop *)data;
  bai_oracle(ξ, ϕ2, size, track_and_stop->is_EV, track_and_stop->oracle_result,
             bai_logger);
  int sample =
      bai_track(track_and_stop->tracking_rule, N,
                track_and_stop->oracle_result->ws_over_Σ, size, bai_logger);
  return sample;
}

typedef int (*next_sample_func_t)(void *, int, int, double *, double *, int *,
                                  double *, double *, int, BAILogger *);

struct BAISamplingRule {
  bai_sampling_rule_t type;
  void *data;
  next_sample_func_t next_sample_func;
};

BAISamplingRule *bai_sampling_rule_create(bai_sampling_rule_t type, int *N,
                                          int size) {
  BAISamplingRule *bai_sampling_rule = malloc_or_die(sizeof(BAISamplingRule));
  bai_sampling_rule->type = type;
  switch (type) {
  case BAI_SAMPLING_RULE_RANDOM:
    log_fatal("BAI_SAMPLING_RULE_RANDOM not implemented");
    break;
  case BAI_SAMPLING_RULE_UNIFORM:
    log_fatal("BAI_SAMPLING_RULE_UNIFORM not implemented");
    break;
  case BAI_SAMPLING_RULE_TRACK_AND_STOP:
    bai_sampling_rule->data =
        track_and_stop_create(true, BAI_CTRACKING, N, size);
    bai_sampling_rule->next_sample_func = track_and_stop_next_sample;
    break;
  case BAI_SAMPLING_RULE_TRACK_AND_STOP_EV:
    bai_sampling_rule->data =
        track_and_stop_create(false, BAI_CTRACKING, N, size);
    bai_sampling_rule->next_sample_func = track_and_stop_next_sample;
    break;
  }
  return bai_sampling_rule;
}

void bai_sampling_rule_destroy(BAISamplingRule *bai_sampling_rule) {
  switch (bai_sampling_rule->type) {
  case BAI_SAMPLING_RULE_RANDOM:
    log_fatal("BAI_SAMPLING_RULE_RANDOM not implemented");
    break;
  case BAI_SAMPLING_RULE_UNIFORM:
    log_fatal("BAI_SAMPLING_RULE_UNIFORM not implemented");
    break;
  case BAI_SAMPLING_RULE_TRACK_AND_STOP_EV:
    track_and_stop_destroy((TrackAndStop *)bai_sampling_rule->data);
    break;
  }
  free(bai_sampling_rule);
}

int bai_sampling_rule_next_sample(BAISamplingRule *bai_sampling_rule, int astar,
                                  int aalt, double *ξ, double *ϕ2, int *N,
                                  double *S, double *Zs, int size,
                                  BAILogger *bai_logger) {
  return bai_sampling_rule->next_sample_func(
      bai_sampling_rule->data, astar, aalt, ξ, ϕ2, N, S, Zs, size, bai_logger);
}

bool bai_sampling_rule_is_ev(bai_sampling_rule_t type) {
  return type == BAI_SAMPLING_RULE_TRACK_AND_STOP_EV;
}