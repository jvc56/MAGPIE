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
  // One additional 2-ply candidate after the calibrated first-two wave. The
  // v1 corpus did not freeze a separate single-candidate tail, so the planner
  // deliberately uses the entire first-two empirical maximum as a
  // conservative proxy and reports that fact in the decision.
  PEG_TIME_MANAGER_BOUNDARY_NEXT_2PLY_CANDIDATE,
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
  // Zero means 1.0 for backwards-compatible shadow callers. The multiplier is
  // applied only to the empirical completion envelope, never to expected work
  // used for value pricing.
  double completion_bound_multiplier;
  // The empirical maximum has 95.99% evidence of covering population p99, not
  // the normal 99% production evidence target. A caller must explicitly opt
  // into enforcing that provisional envelope; otherwise decisions remain
  // shadow-only even when `clock.minimum_completion_confidence` is lowered.
  bool allow_provisional_enforcement;
  // Rescale the supplied local cost models from work already completed in the
  // current solve. This lets the portable calibration adapt to hardware,
  // thermal state, and co-load before admitting the first refinement wave.
  bool use_live_cost_scale;
  // Portable reserve for later same-player endgame work. It is converted with
  // the effective live PEG endgame-node rate and this safety multiplier.
  uint64_t future_reserve_endgame_nodes;
  double future_rate_safety_multiplier;
  // Candidate-wave stopping policy. Zero selects the calibrated defaults
  // (minimum 8, stability patience 4, maximum 32).
  int minimum_2ply_candidates;
  int stability_patience_candidates;
  int maximum_2ply_candidates;
} PegTimeManagerPolicy;

typedef struct PegTimeManagerDecision {
  bool valid;
  bool configuration_matches;
  // False by default. True only for a policy that explicitly accepts the
  // provisional 95.99%-evidence envelope; it is not a certified p99 claim.
  bool safe_to_enforce;
  bool should_start;
  // True for a post-wave candidate because its deadline envelope is the whole
  // first-two maximum, not a separately calibrated single-candidate tail.
  bool uses_post_wave_tail_proxy;
  bool has_provisional_completion_bound;
  TimeManagerWork pricing_work;
  TimeManagerWork empirical_p99_work;
  TimeManagerWork provisional_completion_bound_work;
  double pricing_seconds;
  double empirical_p99_seconds;
  double provisional_completion_bound_seconds;
  double completion_confidence;
  // Multiplicative adjustment inferred from this solve's completed work. 1.0
  // means the caller's supplied model was retained.
  double live_cost_scale;
  double expected_regret_reduction;
  TimeManagerPlan plan;
} PegTimeManagerDecision;

const PegTimeManagerCalibration *
peg_time_manager_default_calibration(PegTimeManagerBoundaryKind boundary_kind,
                                     int bag_tiles);

const PegTimeManagerValueObservation *
peg_time_manager_default_2ply_value_observation(int completed_candidates_before,
                                                int completed_candidates_after);

// Smoothed per-boundary scheduler prior derived from the direct-4 panel. The
// strong 2->4 and 4->8 increments are spread uniformly over their newly
// completed candidates. Beyond eight, use the much smaller measured
// full-32-minus-fixed-8 rare-rescue mean rather than the negative finite-panel
// 8->12 point estimate.
double peg_time_manager_default_regret_reduction(
    const PegTimeManagerRequest *request,
    const PegTimeManagerCalibration *calibration, void *user_data);

// Reconstructs the loaded M5's local cost model, scaled to a caller's local
// wall time. The fitted negative candidate coefficient is clamped to zero;
// the intercept is represented explicitly as a per-wave fixed cost. This
// helper is for reproduction/shadow evaluation, not production calibration.
bool peg_time_manager_reference_cost_models(
    const PegTimeManagerCalibration *calibration, double local_time_scale,
    double deadline_slowdown_multiplier,
    TimeManagerCostModel *expected_cost_model,
    TimeManagerCostModel *deadline_cost_model);

// Forecast the expected wall time to reach the minimum usable 2-ply prefix.
// The first two candidates use the measured pair work. Later candidates use
// the live policy's one-candidate proxy: half the pair's variable work plus a
// fresh per-boundary fixed cost. Returns NAN for an invalid calibration,
// model, or candidate count.
double peg_time_manager_estimate_minimum_2ply_seconds(
    const PegTimeManagerCalibration *calibration,
    const TimeManagerCostModel *expected_cost_model, int minimum_candidates);

// Plans either the exact first-two wave or one subsequent 2-ply candidate. The
// latter keeps the whole pair maximum as a conservative, clearly labeled
// proxy. The 95.99% p99-coverage evidence makes a normal 0.99 policy fail
// closed; enforcement requires an explicit lower threshold and opt-in.
PegTimeManagerDecision
peg_time_manager_plan_boundary(const PegTimeManagerPolicy *policy,
                               const PegTimeManagerRequest *request);

#endif
