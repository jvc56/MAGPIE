#ifndef BAI_DEFS_H
#define BAI_DEFS_H

#include <stdbool.h>
#include <stdint.h>

typedef struct SimResults SimResults;

// A residual-regret estimate is not considered identified until every arm has
// at least this many observations. Keep this public because callers that want
// finite telemetry should use the same value for their initial sampling floor.
#define BAI_MINIMUM_REGRET_SAMPLES_PER_ARM 32

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
  // Optional value-of-computation stopping rule. The BAI estimates the
  // expected utility loss from returning its current best arm and stops once
  // that regret is no greater than regret_stop_target. A nonpositive target
  // disables the rule. Cross-arm correlation models the common random
  // scenarios used by simulation arms; 0 is the independent-arm estimate.
  double regret_stop_target;
  // Experimental selector for the regret-limit boundary. False preserves the
  // deployed max-of-pairwise lower bound. True uses the shadow joint-maximum
  // estimate; production callers leave this false until prospective
  // calibration and optional-stopping gates pass.
  bool regret_stop_use_joint;
  double regret_cross_arm_correlation;
  double regret_calibration;
  uint64_t regret_check_interval;
  uint64_t regret_min_samples_per_arm;
  // Disabled-by-default Rule-of-Zero stop. This is intentionally separate
  // from expected-regret stopping: it may stop only after a fixed minimum
  // amount of native work, a stable incumbent at checkpoint boundaries, and
  // a valid zero near-tie count. A missing SimResults pointer or malformed
  // configuration fails closed and runs to the ordinary solver boundary.
  bool rule_zero_enabled;
  uint64_t rule_zero_minimum_nodes;
  int rule_zero_minimum_stable_checkpoints;
  uint64_t rule_zero_checkpoint_interval;
  const SimResults *rule_zero_sim_results;
  // Array of arm indices to avoid pruning. NULL if none.
  // NOTE: bai() mutates this array in-place via swap-and-shrink during
  // sim_unpruned_to_winner. The caller must not rely on its contents
  // being preserved after bai() returns.
  int *arm_avoid_prune;
  int num_arm_avoid_prune;
} BAIOptions;

#endif
