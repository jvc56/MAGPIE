#ifndef PEG_TIME_MANAGER_H
#define PEG_TIME_MANAGER_H

#include "time_manager.h"
#include <stdbool.h>
#include <stdint.h>

#define PEG_TIME_MANAGER_COMPLETION_ARTIFACT_ID                                \
  "csw24-peg-first2-hc-flat-stride1-shadow-v1-20260730"
#define PEG_TIME_MANAGER_COST_ARTIFACT_ID                                      \
  "csw24-peg-hc-local-wall-shadow-v1-20260730"
#define PEG_TIME_MANAGER_VALUE_ARTIFACT_ID                                     \
  "csw24-peg-direct4-quality-shadow-v1-20260730"

// The first useful PEG result at a new fidelity needs two completed
// candidates: one candidate cannot re-rank anything. Later candidates may be
// admitted singly, but the v1 completion-tail artifact deliberately covers
// only these first-two waves.
typedef enum PegTimeManagerBoundaryKind {
  PEG_TIME_MANAGER_BOUNDARY_INVALID = 0,
  PEG_TIME_MANAGER_BOUNDARY_FIRST_TWO_2PLY,
  PEG_TIME_MANAGER_BOUNDARY_FIRST_TWO_3PLY_AFTER_16,
} PegTimeManagerBoundaryKind;

// Frozen portable work observations from 320 independent, uncensored source
// positions per supported bag/boundary. `median_work` is a provisional pricing
// proxy, not a mean. `empirical_max_work` is the conservative shadow envelope;
// the componentwise empirical p99 is retained separately for diagnostics.
//
// The observed maximum has only 1 - 0.99^320 ~= 95.99% confidence of covering
// the population p99 under exchangeability. It therefore remains below
// TimeManager's normal 0.99 production confidence requirement even though the
// underlying target quantile is p99.
typedef struct PegTimeManagerCalibration {
  PegTimeManagerBoundaryKind boundary_kind;
  int bag_tiles;
  int stage_index;
  int fidelity_plies;
  int reference_workers;
  int candidates;
  int required_completed_2ply_candidates;
  bool nested_enabled;
  int scenario_stride;
  bool parallel_wave_dispatch;
  int complete_positions;
  int censored_positions;
  TimeManagerWork median_work;
  TimeManagerWork empirical_p99_work;
  TimeManagerWork empirical_max_work;
  int heldout_positions;
  int heldout_false_starts;
  double target_completion_quantile;
  double empirical_max_p99_coverage_confidence;
  // Loaded-M5 local wall conversion. This is only a reproducibility anchor;
  // runtime planning should replace it with a live local cost model.
  double reference_fixed_seconds_per_wave;
  double reference_seconds_per_scenario;
  double reference_seconds_per_endgame_node;
  double reference_seconds_per_candidate;
} PegTimeManagerCalibration;

// Direct-4 common-oracle observations for completed 2-ply checkpoints. These
// are raw panel means, including agreement positions as exact zero. Callers
// must shrink/regularize them before using them as a scheduler prior; in
// particular, the finite-sample curve is non-monotone.
typedef struct PegTimeManagerValueObservation {
  int completed_candidates_before;
  int completed_candidates_after;
  int positions;
  int disagreements;
  double mean_incremental_scenarios;
  double mean_incremental_endgame_nodes;
  double mean_incremental_candidates;
  double mean_m5_seconds;
  double observed_utility_gain;
  double observed_utility_ci_low;
  double observed_utility_ci_high;
  double observed_p_value;
} PegTimeManagerValueObservation;

typedef struct PegTimeManagerRequest {
  PegTimeManagerBoundaryKind boundary_kind;
  int bag_tiles;
  int stage_index;
  int fidelity_plies;
  int workers;
  int candidates;
  int completed_2ply_candidates;
  bool nested_enabled;
  int scenario_stride;
  // True when exactly the requested two-candidate wave is submitted behind
  // one barrier. A whole 8/16/32-candidate stage is not a matching wave.
  bool parallel_wave_dispatch;
  // Work/time already spent on this move, normally including the greedy seed.
  uint64_t completed_scenarios;
  uint64_t completed_endgame_nodes;
  double elapsed_seconds;
  // Physical limit on this solver invocation. It remains an independent gate
  // even when TimeManager plans against a larger player clock.
  double remaining_seconds;
  bool has_player_clock;
  double player_clock_remaining_seconds;
} PegTimeManagerRequest;

typedef double (*PegTimeManagerRegretReductionCallback)(
    const PegTimeManagerRequest *request,
    const PegTimeManagerCalibration *calibration, void *user_data);

typedef struct PegTimeManagerPolicy {
  TimeManagerClock clock;
  TimeManagerCostModel expected_cost_model;
  TimeManagerCostModel deadline_cost_model;
  PegTimeManagerRegretReductionCallback regret_callback;
  void *regret_callback_data;
  double fixed_expected_regret_reduction;
} PegTimeManagerPolicy;

typedef struct PegTimeManagerDecision {
  bool valid;
  bool configuration_matches;
  // False for v1: candidate-wave execution, a production safety factor, and
  // online miss telemetry are not all present yet.
  bool safe_to_enforce;
  bool should_start;
  bool has_provisional_completion_bound;
  TimeManagerWork pricing_work;
  TimeManagerWork empirical_p99_work;
  TimeManagerWork provisional_completion_bound_work;
  double pricing_seconds;
  double empirical_p99_seconds;
  double provisional_completion_bound_seconds;
  double completion_confidence;
  double expected_regret_reduction;
  TimeManagerPlan plan;
} PegTimeManagerDecision;

const PegTimeManagerCalibration *
peg_time_manager_default_calibration(PegTimeManagerBoundaryKind boundary_kind,
                                     int bag_tiles);

const PegTimeManagerValueObservation *
peg_time_manager_default_2ply_value_observation(int completed_candidates_before,
                                                int completed_candidates_after);

// Reconstructs the loaded M5's local cost model, scaled to a caller's local
// wall time. The fitted negative candidate coefficient is clamped to zero;
// the intercept is represented explicitly as a per-wave fixed cost. This
// helper is for reproduction/shadow evaluation, not production calibration.
bool peg_time_manager_reference_cost_models(
    const PegTimeManagerCalibration *calibration, double local_time_scale,
    double deadline_slowdown_multiplier,
    TimeManagerCostModel *expected_cost_model,
    TimeManagerCostModel *deadline_cost_model);

// Plans one exactly-two-candidate boundary. The empirical maximum is offered
// as a provisional completion envelope, but its 95.99% p99-coverage evidence
// makes a normal 0.99 policy fail closed. Current production PEG still
// dispatches larger stages; those requests deliberately fail configuration
// matching rather than pretending the first-two bound covers the whole stage.
PegTimeManagerDecision
peg_time_manager_plan_boundary(const PegTimeManagerPolicy *policy,
                               const PegTimeManagerRequest *request);

#endif
