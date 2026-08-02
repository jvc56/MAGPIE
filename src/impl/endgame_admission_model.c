#include "endgame_admission_model.h"

#include "../ent/analysis_progress.h"
#include "endgame_admission_model_default_data.h"
#include <math.h>
#include <stdint.h>

static const EndgameAdmissionDepthModel *
endgame_admission_find_depth_model(const EndgameAdmissionModel *model,
                                   int target_depth) {
  if (model == NULL || model->depth_models == NULL) {
    return NULL;
  }
  for (size_t index = 0; index < model->depth_model_count; index++) {
    if (model->depth_models[index].target_depth == target_depth) {
      return &model->depth_models[index];
    }
  }
  return NULL;
}

static bool endgame_admission_has_last_increment(
    const EndgameDepthAdmissionRequest *request) {
  return request->has_previous &&
         request->previous.depth == request->current.depth - 1 &&
         request->current.cumulative_nodes > request->previous.cumulative_nodes;
}

static bool endgame_admission_has_prior_increment(
    const EndgameDepthAdmissionRequest *request, bool has_last_increment) {
  return has_last_increment && request->has_prior &&
         request->prior.depth == request->previous.depth - 1 &&
         request->previous.cumulative_nodes > request->prior.cumulative_nodes;
}

bool endgame_admission_build_features(
    const EndgameDepthAdmissionRequest *request,
    double features[ENDGAME_ADMISSION_FEATURE_COUNT]) {
  if (request == NULL || features == NULL || request->current.depth <= 0 ||
      request->next_depth != request->current.depth + 1 ||
      request->current.cumulative_nodes == 0 ||
      !isfinite(request->current.elapsed_seconds) ||
      request->current.elapsed_seconds < 0.0 ||
      request->current.root_moves_total < 0 ||
      request->current.ply2_moves_total < 0 || request->player_rack_tiles < 0 ||
      request->opponent_rack_tiles < 0 ||
      request->consecutive_scoreless_turns < 0) {
    return false;
  }

  const bool has_last_increment = endgame_admission_has_last_increment(request);
  const uint64_t last_increment = has_last_increment
                                      ? request->current.cumulative_nodes -
                                            request->previous.cumulative_nodes
                                      : request->current.cumulative_nodes;
  const bool has_prior_increment =
      endgame_admission_has_prior_increment(request, has_last_increment);
  const uint64_t prior_increment =
      has_prior_increment
          ? request->previous.cumulative_nodes - request->prior.cumulative_nodes
          : last_increment;
  const double log_previous_nodes =
      log1p((double)request->current.cumulative_nodes);
  const double log_last_increment = log1p((double)last_increment);
  const double log_prior_increment = log1p((double)prior_increment);
  const double log_root_moves =
      log1p((double)request->current.root_moves_total);
  const double log_ply2_moves =
      log1p((double)request->current.ply2_moves_total);
  const double log_root_per_ply2 =
      log(((double)request->current.root_moves_total + 1.0) /
          ((double)request->current.ply2_moves_total + 1.0));

  features[ENDGAME_ADMISSION_FEATURE_INTERCEPT] = 1.0;
  features[ENDGAME_ADMISSION_FEATURE_RACK_TILES_MINUS_13] =
      (double)(request->player_rack_tiles + request->opponent_rack_tiles - 13);
  features[ENDGAME_ADMISSION_FEATURE_LOG_PREVIOUS_NODES] = log_previous_nodes;
  features[ENDGAME_ADMISSION_FEATURE_LOG_LAST_INCREMENT_NODES] =
      log_last_increment;
  features[ENDGAME_ADMISSION_FEATURE_LAST_INCREMENT_MISSING] =
      has_last_increment ? 0.0 : 1.0;
  features[ENDGAME_ADMISSION_FEATURE_LOG_PRIOR_INCREMENT_NODES] =
      log_prior_increment;
  features[ENDGAME_ADMISSION_FEATURE_PRIOR_INCREMENT_MISSING] =
      has_prior_increment ? 0.0 : 1.0;
  features[ENDGAME_ADMISSION_FEATURE_LOG_ROOT_MOVES] = log_root_moves;
  features[ENDGAME_ADMISSION_FEATURE_LOG_PLY2_MOVES] = log_ply2_moves;
  features[ENDGAME_ADMISSION_FEATURE_LOG_ROOT_PER_PLY2] = log_root_per_ply2;
  features[ENDGAME_ADMISSION_FEATURE_LOG_PREVIOUS_NODES_X_LOG_ROOT_PER_PLY2] =
      log_previous_nodes * log_root_per_ply2;
  features[ENDGAME_ADMISSION_FEATURE_LOG_LAST_INCREMENT_X_LOG_ROOT_PER_PLY2] =
      log_last_increment * log_root_per_ply2;
  features[ENDGAME_ADMISSION_FEATURE_PLAYER_RACK_TILES] =
      (double)request->player_rack_tiles;
  features[ENDGAME_ADMISSION_FEATURE_OPPONENT_RACK_TILES] =
      (double)request->opponent_rack_tiles;
  features[ENDGAME_ADMISSION_FEATURE_SCORE_SPREAD_DIV_100] =
      (double)request->score_spread / 100.0;
  features[ENDGAME_ADMISSION_FEATURE_ABSOLUTE_SCORE_SPREAD_DIV_100] =
      fabs((double)request->score_spread) / 100.0;
  features[ENDGAME_ADMISSION_FEATURE_CONSECUTIVE_SCORELESS_TURNS] =
      (double)request->consecutive_scoreless_turns;
  // A live Endgame request always has these fields; this feature exists only
  // because some historical training rows predated CGP enrichment.
  features[ENDGAME_ADMISSION_FEATURE_POSITION_FEATURES_MISSING] = 0.0;
  features[ENDGAME_ADMISSION_FEATURE_BEST_MOVE_CHANGED] =
      has_last_increment &&
              request->current.best_move != request->previous.best_move
          ? 1.0
          : 0.0;
  features[ENDGAME_ADMISSION_FEATURE_SEARCH_VALUE_CHANGED] =
      has_last_increment && request->current.value != request->previous.value
          ? 1.0
          : 0.0;
  return true;
}

static bool endgame_admission_ceil_to_uint64(double value, uint64_t *output) {
  // 0x1p64 is exactly representable; (double)UINT64_MAX rounds up to it.
  if (!isfinite(value) || value <= 0.0 || value >= 0x1p64) {
    return false;
  }
  *output = (uint64_t)ceil(value);
  return true;
}

static double
endgame_admission_live_nps(const EndgameDepthAdmissionRequest *request) {
  if (endgame_admission_has_last_increment(request) &&
      isfinite(request->previous.elapsed_seconds) &&
      request->current.elapsed_seconds > request->previous.elapsed_seconds) {
    const double seconds =
        request->current.elapsed_seconds - request->previous.elapsed_seconds;
    const uint64_t nodes =
        request->current.cumulative_nodes - request->previous.cumulative_nodes;
    return (double)nodes / seconds;
  }
  if (request->current.elapsed_seconds > 0.0) {
    return (double)request->current.cumulative_nodes /
           request->current.elapsed_seconds;
  }
  return NAN;
}

EndgameAdmissionPrediction
endgame_admission_model_predict(const EndgameAdmissionModel *model,
                                const EndgameDepthAdmissionRequest *request) {
  EndgameAdmissionPrediction prediction = {0};
  if (model == NULL || request == NULL ||
      model->artifact_version != ENDGAME_ADMISSION_MODEL_ARTIFACT_VERSION ||
      model->feature_schema_version !=
          ENDGAME_ADMISSION_FEATURE_SCHEMA_VERSION ||
      !model->target_ratio || !isfinite(model->completion_confidence) ||
      model->completion_confidence <= 0.0 ||
      model->completion_confidence > 1.0) {
    return prediction;
  }
  const EndgameAdmissionDepthModel *depth_model =
      endgame_admission_find_depth_model(model, request->next_depth);
  if (depth_model == NULL ||
      !isfinite(depth_model->node_correction_multiplier) ||
      depth_model->node_correction_multiplier < 1.0 ||
      !isfinite(depth_model->wall_correction_multiplier) ||
      depth_model->wall_correction_multiplier < 1.0) {
    return prediction;
  }
  double features[ENDGAME_ADMISSION_FEATURE_COUNT];
  if (!endgame_admission_build_features(request, features)) {
    return prediction;
  }

  double predicted_log_nodes = 0.0;
  for (int index = 0; index < ENDGAME_ADMISSION_FEATURE_COUNT; index++) {
    double standardized = 1.0;
    if (index > 0) {
      if (!isfinite(depth_model->means[index]) ||
          !isfinite(depth_model->scales[index]) ||
          depth_model->scales[index] <= 0.0) {
        return prediction;
      }
      standardized = (features[index] - depth_model->means[index]) /
                     depth_model->scales[index];
    }
    if (!isfinite(depth_model->coefficients[index])) {
      return prediction;
    }
    predicted_log_nodes += depth_model->coefficients[index] * standardized;
  }
  predicted_log_nodes += features[ENDGAME_ADMISSION_FEATURE_LOG_PREVIOUS_NODES];
  const double expected_nodes = exp(predicted_log_nodes);
  const double completion_bound_nodes =
      expected_nodes * depth_model->node_correction_multiplier;
  if (!endgame_admission_ceil_to_uint64(expected_nodes,
                                        &prediction.expected_nodes) ||
      !endgame_admission_ceil_to_uint64(completion_bound_nodes,
                                        &prediction.completion_bound_nodes)) {
    return (EndgameAdmissionPrediction){0};
  }

  prediction.valid = true;
  prediction.worker_count_matches = request->workers == model->workers;
  prediction.boundary_protocol_matches = depth_model->enforceable;
  prediction.completion_confidence = model->completion_confidence;
  prediction.live_nodes_per_second = endgame_admission_live_nps(request);
  if (isfinite(prediction.live_nodes_per_second) &&
      prediction.live_nodes_per_second > 0.0) {
    prediction.has_wall_prediction = true;
    prediction.expected_seconds =
        expected_nodes / prediction.live_nodes_per_second;
    prediction.completion_bound_seconds =
        prediction.expected_seconds * depth_model->wall_correction_multiplier;
    if (!isfinite(prediction.expected_seconds) ||
        !isfinite(prediction.completion_bound_seconds)) {
      prediction.has_wall_prediction = false;
      prediction.expected_seconds = 0.0;
      prediction.completion_bound_seconds = 0.0;
    }
  }
  return prediction;
}

EndgameDepthAdmissionDecision
endgame_admission_model_callback(const EndgameDepthAdmissionRequest *request,
                                 void *user_data) {
  const EndgameAdmissionPolicy *policy = user_data;
  if (policy == NULL || policy->model == NULL ||
      !isfinite(policy->reserve_seconds) || policy->reserve_seconds < 0.0) {
    return (EndgameDepthAdmissionDecision){0};
  }
  const EndgameAdmissionPrediction prediction =
      endgame_admission_model_predict(policy->model, request);
  if (!prediction.valid) {
    return (EndgameDepthAdmissionDecision){0};
  }

  const bool tt_matches = !policy->uses_external_tt &&
                          fabs(policy->tt_fraction_of_mem -
                               policy->model->tt_fraction_of_mem) <= 1.0e-12;
  const bool configuration_matches =
      prediction.worker_count_matches && prediction.boundary_protocol_matches &&
      tt_matches && policy->use_heuristics == policy->model->use_heuristics;
  bool has_applicable_budget = false;
  bool should_start = true;
  if (request->node_limit > 0) {
    has_applicable_budget = true;
    should_start = should_start && prediction.completion_bound_nodes <=
                                       request->remaining_nodes;
  }
  if (isfinite(request->remaining_seconds)) {
    if (!prediction.has_wall_prediction) {
      return (EndgameDepthAdmissionDecision){0};
    }
    has_applicable_budget = true;
    const double available_seconds =
        fmax(0.0, request->remaining_seconds - policy->reserve_seconds);
    should_start = should_start &&
                   prediction.completion_bound_seconds <= available_seconds;
  }
  // An unbounded fixed-depth solve should continue; the model still supplies
  // its work prediction for trace/audit purposes.
  if (!has_applicable_budget) {
    should_start = true;
  }
  return (EndgameDepthAdmissionDecision){
      .valid = true,
      .safe_to_enforce = configuration_matches,
      .should_start = should_start,
      .expected_nodes = prediction.expected_nodes,
      .completion_bound_nodes = prediction.completion_bound_nodes,
      .expected_seconds =
          prediction.has_wall_prediction ? prediction.expected_seconds : 0.0,
      .completion_bound_seconds = prediction.has_wall_prediction
                                      ? prediction.completion_bound_seconds
                                      : 0.0,
      .completion_confidence = prediction.completion_confidence,
  };
}

EndgameDepthAdmissionDecision endgame_admission_time_manager_callback(
    const EndgameDepthAdmissionRequest *request, void *user_data) {
  EndgameAdmissionTimeManagerPolicy *policy = user_data;
  if (request == NULL || policy == NULL) {
    return (EndgameDepthAdmissionDecision){0};
  }
  policy->has_last_plan = false;

  // TimeManager owns the reserve calculation. Ask the predictor for the raw
  // fit decision, then add the predictor policy's legacy reserve to the
  // TimeManager safety reserve exactly once below.
  EndgameAdmissionPolicy prediction_policy = policy->prediction_policy;
  prediction_policy.reserve_seconds = 0.0;
  EndgameDepthAdmissionDecision decision =
      endgame_admission_model_callback(request, &prediction_policy);
  if (!decision.valid) {
    return decision;
  }

  const double planning_remaining_seconds =
      request->has_player_clock ? request->player_clock_remaining_seconds
                                : request->remaining_seconds;
  if (request->has_player_clock && (!isfinite(planning_remaining_seconds) ||
                                    planning_remaining_seconds < 0.0)) {
    return (EndgameDepthAdmissionDecision){0};
  }
  // An unbounded fixed-depth analysis with no player clock has no cross-turn
  // allocation problem. Keep the predictor's normal behavior and still
  // expose its work estimate.
  if (!isfinite(planning_remaining_seconds)) {
    return decision;
  }
  if (decision.expected_seconds <= 0.0 ||
      decision.completion_bound_seconds < decision.expected_seconds ||
      decision.expected_nodes == 0 ||
      decision.completion_bound_nodes < decision.expected_nodes) {
    return (EndgameDepthAdmissionDecision){0};
  }

  double expected_regret_reduction = policy->fixed_expected_regret_reduction;
  if (policy->regret_callback != NULL) {
    expected_regret_reduction = policy->regret_callback(
        request, &decision, policy->regret_callback_data);
  }
  if (!isfinite(expected_regret_reduction) || expected_regret_reduction < 0.0) {
    return (EndgameDepthAdmissionDecision){0};
  }

  TimeManagerClock clock = policy->clock;
  clock.remaining_seconds = planning_remaining_seconds;
  clock.safety_reserve_seconds += policy->prediction_policy.reserve_seconds;
  if (policy->future_reserve_nodes > 0) {
    if (!isfinite(policy->future_rate_safety_multiplier) ||
        policy->future_rate_safety_multiplier < 1.0) {
      return (EndgameDepthAdmissionDecision){0};
    }
    const double live_seconds_per_node =
        decision.expected_seconds / (double)decision.expected_nodes;
    const double node_reserve_seconds = (double)policy->future_reserve_nodes *
                                        live_seconds_per_node *
                                        policy->future_rate_safety_multiplier;
    if (!isfinite(node_reserve_seconds) || node_reserve_seconds < 0.0) {
      return (EndgameDepthAdmissionDecision){0};
    }
    clock.future_reserve_seconds += node_reserve_seconds;
  }
  if (!isfinite(clock.safety_reserve_seconds)) {
    return (EndgameDepthAdmissionDecision){0};
  }
  if (!isfinite(clock.future_reserve_seconds)) {
    return (EndgameDepthAdmissionDecision){0};
  }

  // Preserve the model's joint wall calibration exactly while retaining
  // hardware-independent node coordinates in the TimeManager chunk.
  const TimeManagerCostModel expected_cost_model = {
      .endgame_seconds_per_node =
          decision.expected_seconds / (double)decision.expected_nodes,
  };
  const TimeManagerCostModel deadline_cost_model = {
      .endgame_seconds_per_node = decision.completion_bound_seconds /
                                  (double)decision.completion_bound_nodes,
  };
  const TimeManagerChunk chunk = {
      .boundary = TIME_MANAGER_BOUNDARY_ENDGAME_DEPTH,
      .work =
          {
              .mode = ANALYSIS_MODE_ENDGAME,
              .nodes = decision.expected_nodes,
          },
      .has_completion_bound = true,
      .completion_bound_work =
          {
              .mode = ANALYSIS_MODE_ENDGAME,
              .nodes = decision.completion_bound_nodes,
          },
      .completion_confidence = decision.completion_confidence,
      .expected_regret_reduction = expected_regret_reduction,
  };
  policy->last_plan = time_manager_plan(&clock, &expected_cost_model,
                                        &deadline_cost_model, &chunk, 1);
  policy->has_last_plan = true;
  if (!policy->last_plan.valid) {
    return (EndgameDepthAdmissionDecision){0};
  }

  // The raw node/time checks and the cross-turn plan are both required.
  decision.should_start =
      decision.should_start && policy->last_plan.chunks_bought == 1;
  decision.has_time_manager_plan = true;
  decision.expected_regret_reduction = expected_regret_reduction;
  decision.current_value_per_second =
      expected_regret_reduction / decision.expected_seconds;
  decision.future_value_per_second = clock.future_value_per_second;
  decision.maximum_current_seconds = policy->last_plan.maximum_current_seconds;
  decision.deposit_seconds = policy->last_plan.deposit_seconds;
  return decision;
}

double endgame_admission_target5_regret_prior(
    const EndgameDepthAdmissionRequest *request,
    const EndgameDepthAdmissionDecision *prediction, void *user_data) {
  (void)user_data;
  if (request == NULL || prediction == NULL || !prediction->valid ||
      request->next_depth != 5 || request->current.depth != 4) {
    return NAN;
  }
  if (!request->has_previous ||
      request->previous.depth != request->current.depth - 1) {
    // No-history positions had zero observed gain in the 28-position
    // validation stratum. Keep an explicit exploration floor so an ample-clock
    // policy may still buy the depth; this is not an empirical mean.
    return 0.000001;
  }
  const bool settled =
      request->current.best_move == request->previous.best_move &&
      request->current.value == request->previous.value;
  // Independent common-depth-6 validation, excluding four capped arms and two
  // capped judges: settled n=177, unsettled n=89. These are mean reductions in
  // utility regret from completing depth 5, not probabilities that the current
  // move is best.
  return settled ? 0.000005649717514124294 : 0.000026966292134831465;
}

uint64_t endgame_future_depth5_reserve_nodes(int combined_rack_tiles) {
  // Pooled observed maxima for all later same-player depth-5 solves, made
  // monotone so removing tiles can never increase the reserved work. The
  // first materially hard tail appears around ten combined rack tiles.
  static const uint64_t reserve_nodes_by_tiles[2 * RACK_SIZE + 1] = {
      UINT64_C(0),       UINT64_C(0),        UINT64_C(0),
      UINT64_C(10),      UINT64_C(3784),     UINT64_C(24043),
      UINT64_C(59186),   UINT64_C(177893),   UINT64_C(545995),
      UINT64_C(771026),  UINT64_C(3121816),  UINT64_C(3121816),
      UINT64_C(5819156), UINT64_C(13501513), UINT64_C(13501513),
  };
  if (combined_rack_tiles < 0) {
    return UINT64_MAX;
  }
  if (combined_rack_tiles > 2 * RACK_SIZE) {
    combined_rack_tiles = 2 * RACK_SIZE;
  }
  return reserve_nodes_by_tiles[combined_rack_tiles];
}

const EndgameAdmissionModel *endgame_admission_default_model(void) {
  return &ENDGAME_ADMISSION_DEFAULT_MODEL;
}
