#ifndef ENDGAME_ADMISSION_MODEL_H
#define ENDGAME_ADMISSION_MODEL_H

#include "endgame.h"
#include "time_manager.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ENDGAME_ADMISSION_MODEL_ARTIFACT_VERSION 1U
#define ENDGAME_ADMISSION_FEATURE_SCHEMA_VERSION 1U
#define ENDGAME_ADMISSION_TARGET5_VALUE_PRIOR_ID                               \
  "csw24-eg-target5-judge6-300-shadow-v1-20260729"
#define ENDGAME_FUTURE_DEPTH5_RESERVE_ID                                       \
  "csw24-eg-future-depth5-observed-max-shadow-v0-20260729"

// This order is part of the runtime-artifact schema. Keep it exactly in sync
// with FEATURE_NAMES and observation_for in analyze_endgame_admission.py.
typedef enum EndgameAdmissionFeature {
  ENDGAME_ADMISSION_FEATURE_INTERCEPT,
  ENDGAME_ADMISSION_FEATURE_RACK_TILES_MINUS_13,
  ENDGAME_ADMISSION_FEATURE_LOG_PREVIOUS_NODES,
  ENDGAME_ADMISSION_FEATURE_LOG_LAST_INCREMENT_NODES,
  ENDGAME_ADMISSION_FEATURE_LAST_INCREMENT_MISSING,
  ENDGAME_ADMISSION_FEATURE_LOG_PRIOR_INCREMENT_NODES,
  ENDGAME_ADMISSION_FEATURE_PRIOR_INCREMENT_MISSING,
  ENDGAME_ADMISSION_FEATURE_LOG_ROOT_MOVES,
  ENDGAME_ADMISSION_FEATURE_LOG_PLY2_MOVES,
  ENDGAME_ADMISSION_FEATURE_LOG_ROOT_PER_PLY2,
  ENDGAME_ADMISSION_FEATURE_LOG_PREVIOUS_NODES_X_LOG_ROOT_PER_PLY2,
  ENDGAME_ADMISSION_FEATURE_LOG_LAST_INCREMENT_X_LOG_ROOT_PER_PLY2,
  ENDGAME_ADMISSION_FEATURE_PLAYER_RACK_TILES,
  ENDGAME_ADMISSION_FEATURE_OPPONENT_RACK_TILES,
  ENDGAME_ADMISSION_FEATURE_SCORE_SPREAD_DIV_100,
  ENDGAME_ADMISSION_FEATURE_ABSOLUTE_SCORE_SPREAD_DIV_100,
  ENDGAME_ADMISSION_FEATURE_CONSECUTIVE_SCORELESS_TURNS,
  ENDGAME_ADMISSION_FEATURE_POSITION_FEATURES_MISSING,
  ENDGAME_ADMISSION_FEATURE_BEST_MOVE_CHANGED,
  ENDGAME_ADMISSION_FEATURE_SEARCH_VALUE_CHANGED,
  ENDGAME_ADMISSION_FEATURE_COUNT,
} EndgameAdmissionFeature;

typedef struct EndgameAdmissionDepthModel {
  int target_depth;
  // False when gating this depth would change the ABDADA startup protocol used
  // to collect its training target. Such a prediction is shadow-only.
  bool enforceable;
  double means[ENDGAME_ADMISSION_FEATURE_COUNT];
  double scales[ENDGAME_ADMISSION_FEATURE_COUNT];
  double coefficients[ENDGAME_ADMISSION_FEATURE_COUNT];
  // Multipliers on the point prediction. The wall multiplier was calibrated
  // jointly after converting the prediction with the preceding depth's live
  // NPS; it is not the product of independent p99 node and rate bounds.
  double node_correction_multiplier;
  double wall_correction_multiplier;
} EndgameAdmissionDepthModel;

typedef struct EndgameAdmissionModel {
  uint32_t artifact_version;
  uint32_t feature_schema_version;
  // Provenance only. A progress-schema change does not invalidate this model
  // unless it also changes ENDGAME_ADMISSION_FEATURE_SCHEMA_VERSION.
  uint32_t source_progress_schema_version;
  const char *artifact_id;
  // ABDADA work is worker-count dependent. A different count is outside the
  // artifact's calibrated domain even though the feature calculation remains
  // well-defined.
  int workers;
  double tt_fraction_of_mem;
  bool use_heuristics;
  bool target_ratio;
  double completion_confidence;
  size_t depth_model_count;
  const EndgameAdmissionDepthModel *depth_models;
} EndgameAdmissionModel;

typedef struct EndgameAdmissionPrediction {
  bool valid;
  bool worker_count_matches;
  bool boundary_protocol_matches;
  uint64_t expected_nodes;
  uint64_t completion_bound_nodes;
  // Wall values are available when a positive live NPS can be inferred from
  // the current or preceding completed-depth boundary.
  bool has_wall_prediction;
  double live_nodes_per_second;
  double expected_seconds;
  double completion_bound_seconds;
  double completion_confidence;
} EndgameAdmissionPrediction;

// Runtime policy wrapped around the pure work predictor. Configuration
// mismatch is deliberately separable from prediction validity: shadow mode
// can audit drift, but an enforced decision is marked unsafe.
typedef struct EndgameAdmissionPolicy {
  const EndgameAdmissionModel *model;
  double tt_fraction_of_mem;
  bool use_heuristics;
  bool uses_external_tt;
  double reserve_seconds;
} EndgameAdmissionPolicy;

// Position-aware estimate of the utility regret removed by completing the
// proposed next depth. This is deliberately separate from the completion
// predictor: work and value are calibrated from different held-out targets.
typedef double (*EndgameAdmissionRegretReductionCallback)(
    const EndgameDepthAdmissionRequest *request,
    const EndgameDepthAdmissionDecision *prediction, void *user_data);

// Runtime bridge from the frozen next-depth predictor to TimeManager. The
// callback replans exactly one sequential depth at each completed boundary;
// callers must not pre-authorize several depths from one plan.
//
// `clock.remaining_seconds` is replaced with the player's live game clock
// when the request supplies one, otherwise with the current solver window for
// standalone analyses. The current solver window still independently gates
// whether the proposed depth can physically finish.
// `prediction_policy.reserve_seconds` is added to the clock's safety reserve,
// so it is protected exactly once. When regret_callback is NULL,
// fixed_expected_regret_reduction is used.
//
// `future_reserve_nodes` keeps the phase forecast hardware-independent. When
// nonzero, it is translated with the live seconds/node inferred at this
// boundary, multiplied by `future_rate_safety_multiplier`, then added to the
// clock's explicit future_reserve_seconds. A nonzero node reserve requires a
// finite multiplier >= 1.
//
// The solver serializes admission callbacks. last_plan is therefore safe for
// the owner to inspect after a solve or from a synchronous progress hook, but
// it is diagnostic state rather than part of the admission contract.
typedef struct EndgameAdmissionTimeManagerPolicy {
  EndgameAdmissionPolicy prediction_policy;
  TimeManagerClock clock;
  EndgameAdmissionRegretReductionCallback regret_callback;
  void *regret_callback_data;
  double fixed_expected_regret_reduction;
  uint64_t future_reserve_nodes;
  double future_rate_safety_multiplier;
  TimeManagerPlan last_plan;
  bool has_last_plan;
} EndgameAdmissionTimeManagerPolicy;

// Exposed for Python/C golden parity tests and trace diagnostics.
bool endgame_admission_build_features(
    const EndgameDepthAdmissionRequest *request,
    double features[ENDGAME_ADMISSION_FEATURE_COUNT]);

EndgameAdmissionPrediction
endgame_admission_model_predict(const EndgameAdmissionModel *model,
                                const EndgameDepthAdmissionRequest *request);

EndgameDepthAdmissionDecision
endgame_admission_model_callback(const EndgameDepthAdmissionRequest *request,
                                 void *user_data);

EndgameDepthAdmissionDecision endgame_admission_time_manager_callback(
    const EndgameDepthAdmissionRequest *request, void *user_data);

// Frozen before the separate 300-position audit. This exploratory target-5
// value prior is appropriate only for TimeManager shadow traces until that
// audit is complete; it is not an enforcement policy.
double endgame_admission_target5_regret_prior(
    const EndgameDepthAdmissionRequest *request,
    const EndgameDepthAdmissionDecision *prediction, void *user_data);

// Exploratory shadow-only reserve for all later same-player fixed-depth-5
// endgame solves, indexed by the current combined rack-tile count. The table
// is the monotone envelope of observed maxima from two independent 60-game
// trajectory corpora; it is not a certified population p99.
uint64_t endgame_future_depth5_reserve_nodes(int combined_rack_tiles);

const EndgameAdmissionModel *endgame_admission_default_model(void);

#endif
