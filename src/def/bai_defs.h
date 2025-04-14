#ifndef BAI_DEFS_H
#define BAI_DEFS_H

#include <stdbool.h>

#define BAI_EPSILON 1e-10

typedef enum {
  BAI_THRESHOLD_NONE,
  BAI_THRESHOLD_GK16,
  BAI_THRESHOLD_HT,
} bai_threshold_t;

typedef enum {
  BAI_SAMPLING_RULE_ROUND_ROBIN,
  BAI_SAMPLING_RULE_TRACK_AND_STOP,
  BAI_SAMPLING_RULE_TOP_TWO,
} bai_sampling_rule_t;

typedef enum { BAI_CTRACKING, BAI_DTRACKING } bai_tracking_t;

typedef struct BAIOptions {
  bai_sampling_rule_t sampling_rule;
  bai_threshold_t threshold;
  bai_tracking_t tracking;
  double delta;
  bool is_EV;
  // If the sampling rule is round robin complete, this limit
  // refers to the total number of complete round robins instead
  // of the total number of samples.
  int sample_limit;
  int similar_play_cutoff;
  int time_limit_seconds;
} BAIOptions;

typedef enum {
  BAI_EXIT_CONDITION_NONE,
  BAI_EXIT_CONDITION_THRESHOLD,
  BAI_EXIT_CONDITION_SAMPLE_LIMIT,
  BAI_EXIT_CONDITION_TIME_LIMIT,
  BAI_EXIT_CONDITION_USER_INTERRUPT,
  BAI_EXIT_CONDITION_ONE_ARM_REMAINING,
} bai_exit_cond_t;

typedef struct BAIResult {
  bai_exit_cond_t exit_cond;
  int best_arm;
  int total_samples;
} BAIResult;

#endif