#ifndef BAI_DEFS_H
#define BAI_DEFS_H

#include <stdbool.h>
#include <stdint.h>

enum {
  // Sample values whose sums must not depend on the order samples complete in
  // are accumulated as integers scaled by 2^BAI_FIXED_POINT_SHIFT: BAI's arm
  // statistics (bai.h) and the sim means that move selection ranks by
  // (sim_results.c). Integer addition is exactly commutative and associative;
  // double addition is not, so an identical multiset of samples summed in a
  // different order gives last-bit-different means, which can tip the
  // comparison between two near-tied arms. Both sides share this one scale,
  // and fold the same samples, so BAI's mean for an arm and the sim's mean
  // for the same play are bit-identical and a near-tie breaks the same way in
  // both.
  BAI_FIXED_POINT_SHIFT = 30,
};

#define BAI_FIXED_POINT_SCALE ((int64_t)1 << BAI_FIXED_POINT_SHIFT)

typedef enum {
  BAI_THRESHOLD_NONE,
  BAI_THRESHOLD_GK16,
} bai_threshold_t;

#define BAI_THRESHOLD_NONE_STRING "none"
#define BAI_THRESHOLD_GK16_STRING "gk16"

typedef enum {
  BAI_SAMPLING_RULE_ROUND_ROBIN,
  BAI_SAMPLING_RULE_TOP_TWO_IDS,
} bai_sampling_rule_t;

#define BAI_SAMPLING_RULE_ROUND_ROBIN_STRING "rr"
#define BAI_SAMPLING_RULE_TOP_TWO_IDS_STRING "tt"

typedef struct BAIOptions {
  bai_sampling_rule_t sampling_rule;
  bai_threshold_t threshold;
  double delta;
  uint64_t sample_limit;
  uint64_t sample_minimum;
  double time_limit_seconds;
  int num_threads;
  int parent_worker_thread_index;
  double cutoff;
  // Array of arm indices to avoid pruning. NULL if none.
  // NOTE: bai() mutates this array in-place via swap-and-shrink during
  // sim_unpruned_to_winner. The caller must not rely on its contents
  // being preserved after bai() returns.
  int *arm_avoid_prune;
  int num_arm_avoid_prune;
  // The draws one sample averages, such as a bag-cycled sim sample's
  // rotations; 0 or 1 when each sample is a single draw. Rounds are sized in
  // draws (see BAI_SCHEDULE_ROUND_SIZE), so samples that average many draws
  // still get an adaptive schedule at small budgets.
  uint64_t draws_per_sample;
} BAIOptions;

#endif