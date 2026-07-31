#include "endgame_admission_model_test.h"

#include "../src/impl/endgame_admission_model.h"
#include <assert.h>
#include <math.h>
#include <string.h>

static void assert_relative_near(double actual, double expected) {
  const double tolerance = 1.0e-11 * fmax(1.0, fabs(expected));
  assert(fabs(actual - expected) <= tolerance);
}

static EndgameDepthAdmissionRequest depth4_golden_request(void) {
  return (EndgameDepthAdmissionRequest){
      .current =
          {
              .depth = 3,
              .cumulative_nodes = 5333719,
              .elapsed_seconds = 5.567448,
              .root_moves_total = 5860,
              .ply2_moves_total = 421,
              .best_move = UINT64_C(351273479),
              .value = 26,
          },
      .previous =
          {
              .depth = 2,
              .cumulative_nodes = 241642,
              .elapsed_seconds = 0.422125,
              .root_moves_total = 5860,
              .ply2_moves_total = 347,
              .best_move = UINT64_C(93497234297027),
              .value = 18,
          },
      .prior =
          {
              .depth = 1,
              .cumulative_nodes = 191205,
              .elapsed_seconds = 0.343363,
              .root_moves_total = 5860,
              .ply2_moves_total = 0,
              .best_move = UINT64_C(351273479),
              .value = 30,
          },
      .has_previous = true,
      .has_prior = true,
      .next_depth = 4,
      .requested_plies = 6,
      .workers = 10,
      .player_rack_tiles = 7,
      .opponent_rack_tiles = 6,
      .score_spread = 81,
      .consecutive_scoreless_turns = 0,
  };
}

static void test_endgame_admission_python_c_golden(void) {
  const EndgameAdmissionModel *model = endgame_admission_default_model();
  assert(model != NULL);
  assert(model->artifact_version == ENDGAME_ADMISSION_MODEL_ARTIFACT_VERSION);
  assert(model->feature_schema_version ==
         ENDGAME_ADMISSION_FEATURE_SCHEMA_VERSION);
  assert(strcmp(model->artifact_id, "csw24-eg-ridge-p99-10t-v1-20260729") == 0);
  assert(model->workers == 10);
  assert(model->use_heuristics);
  assert(model->target_ratio);
  assert(model->depth_model_count == 2);
  assert_relative_near(model->completion_confidence, 0.99);

  EndgameDepthAdmissionRequest request = depth4_golden_request();
  double features[ENDGAME_ADMISSION_FEATURE_COUNT];
  assert(endgame_admission_build_features(&request, features));
  const double expected_features[ENDGAME_ADMISSION_FEATURE_COUNT] = {
      1.0,
      0.0,
      15.489559488907947,
      15.443196556688225,
      0.0,
      10.828500138222152,
      0.0,
      8.676075516476429,
      6.045005314036012,
      2.6310702024404176,
      40.754118420193926,
      40.63213429073285,
      7.0,
      6.0,
      0.81,
      0.81,
      0.0,
      0.0,
      1.0,
      1.0,
  };
  for (int index = 0; index < ENDGAME_ADMISSION_FEATURE_COUNT; index++) {
    assert_relative_near(features[index], expected_features[index]);
  }

  EndgameAdmissionPrediction prediction =
      endgame_admission_model_predict(model, &request);
  assert(prediction.valid);
  assert(prediction.worker_count_matches);
  assert(!prediction.boundary_protocol_matches);
  assert(prediction.expected_nodes == UINT64_C(2836946));
  assert(prediction.completion_bound_nodes == UINT64_C(57933029));
  assert(prediction.has_wall_prediction);
  assert_relative_near(prediction.live_nodes_per_second, 989651.572894452);
  assert_relative_near(prediction.expected_seconds, 2.8666103348978713);
  assert_relative_near(prediction.completion_bound_seconds, 35.256000158385284);

  request.workers = 8;
  prediction = endgame_admission_model_predict(model, &request);
  assert(prediction.valid);
  assert(!prediction.worker_count_matches);
}

static void test_endgame_admission_policy(void) {
  EndgameDepthAdmissionRequest request = depth4_golden_request();
  request.remaining_seconds = 100.0;
  request.node_limit = UINT64_C(100000000);
  request.remaining_nodes = UINT64_C(90000000);
  EndgameAdmissionPolicy policy = {
      .model = endgame_admission_default_model(),
      .tt_fraction_of_mem = 0.05,
      .use_heuristics = true,
      .uses_external_tt = false,
      .reserve_seconds = 1.0,
  };
  EndgameDepthAdmissionDecision decision =
      endgame_admission_model_callback(&request, &policy);
  assert(decision.valid);
  // Depth 4 was part of ABDADA's jittered startup in the calibration runs.
  // Refusing it at depth 3 would change the work distribution, so this model
  // is intentionally shadow-only even under the otherwise matching config.
  assert(!decision.safe_to_enforce);
  assert(decision.should_start);
  assert(decision.expected_nodes == UINT64_C(2836946));
  assert_relative_near(decision.completion_bound_seconds, 35.256000158385284);

  request.remaining_seconds = 30.0;
  decision = endgame_admission_model_callback(&request, &policy);
  assert(decision.valid);
  assert(!decision.safe_to_enforce);
  assert(!decision.should_start);

  request.remaining_seconds = 100.0;
  request.remaining_nodes = UINT64_C(50000000);
  decision = endgame_admission_model_callback(&request, &policy);
  assert(!decision.should_start);

  // The PlayChooser currently uses an external shared TT rather than the
  // artifact's calibrated 5%-memory private table. The prediction remains
  // available for shadow auditing but cannot drive an enforced gate.
  policy.uses_external_tt = true;
  request.remaining_nodes = UINT64_C(90000000);
  decision = endgame_admission_model_callback(&request, &policy);
  assert(decision.valid);
  assert(!decision.safe_to_enforce);
  assert(decision.should_start);
}

static void test_endgame_admission_time_manager_bridge(void) {
  EndgameDepthAdmissionRequest request = depth4_golden_request();
  request.remaining_seconds = 100.0;
  request.node_limit = UINT64_C(100000000);
  request.remaining_nodes = UINT64_C(90000000);
  EndgameAdmissionTimeManagerPolicy policy = {
      .prediction_policy =
          {
              .model = endgame_admission_default_model(),
              .tt_fraction_of_mem = 0.05,
              .use_heuristics = true,
              .uses_external_tt = false,
              .reserve_seconds = 2.0,
          },
      .clock =
          {
              .turns_remaining = 4,
              .safety_reserve_seconds = 1.0,
              .future_reserve_seconds = 20.0,
              .committed_current_seconds = 0.25,
              .future_value_per_second = 0.001,
              .minimum_completion_confidence = 0.99,
          },
      .fixed_expected_regret_reduction = 0.02,
  };

  EndgameDepthAdmissionDecision decision =
      endgame_admission_time_manager_callback(&request, &policy);
  assert(decision.valid);
  assert(decision.should_start);
  assert(policy.has_last_plan);
  assert(policy.last_plan.valid);
  assert(policy.last_plan.chunks_bought == 1);
  assert(policy.last_plan.stop_reason == TIME_MANAGER_STOP_NO_MORE_CHUNKS);
  assert(decision.has_time_manager_plan);
  assert_relative_near(decision.expected_regret_reduction, 0.02);
  assert_relative_near(decision.current_value_per_second,
                       0.02 / decision.expected_seconds);
  assert_relative_near(decision.future_value_per_second, 0.001);
  assert_relative_near(decision.maximum_current_seconds, 77.0);
  // The predictor reserve is added to, rather than replacing or duplicating,
  // TimeManager's safety reserve.
  assert_relative_near(policy.last_plan.maximum_current_seconds, 77.0);
  assert_relative_near(policy.last_plan.planned_seconds,
                       0.25 + decision.expected_seconds);
  assert_relative_near(policy.last_plan.planned_completion_bound_seconds,
                       0.25 + decision.completion_bound_seconds);
  assert_relative_near(decision.deposit_seconds,
                       policy.last_plan.deposit_seconds);

  // A portable future-work reserve is converted with this solve's observed
  // rate, with an explicit lower-throughput safety multiplier.
  policy.future_reserve_nodes = UINT64_C(1000000);
  policy.future_rate_safety_multiplier = 1.5;
  decision = endgame_admission_time_manager_callback(&request, &policy);
  assert(decision.valid);
  const double node_reserve_seconds =
      1.5e6 * decision.expected_seconds / (double)decision.expected_nodes;
  assert_relative_near(decision.maximum_current_seconds,
                       77.0 - node_reserve_seconds);
  policy.future_rate_safety_multiplier = 0.5;
  decision = endgame_admission_time_manager_callback(&request, &policy);
  assert(!decision.valid);
  policy.future_reserve_nodes = 0;
  policy.future_rate_safety_multiplier = 0.0;

  // The player's game clock is not the current solver window. TimeManager may
  // see room to withdraw from a 200-second clock, while the raw completion
  // gate still refuses a depth that cannot fit this call's 30-second window.
  request.has_player_clock = true;
  request.player_clock_remaining_seconds = 200.0;
  decision = endgame_admission_time_manager_callback(&request, &policy);
  assert(decision.valid);
  assert(decision.should_start);
  assert_relative_near(decision.maximum_current_seconds, 177.0);
  request.remaining_seconds = 30.0;
  decision = endgame_admission_time_manager_callback(&request, &policy);
  assert(decision.valid);
  assert(!decision.should_start);
  assert(policy.last_plan.chunks_bought == 1);
  request.remaining_seconds = 100.0;
  request.has_player_clock = false;
  request.player_clock_remaining_seconds = 0.0;

  // The depth can fit and still lose to more valuable future computation.
  policy.clock.future_value_per_second = 0.01;
  decision = endgame_admission_time_manager_callback(&request, &policy);
  assert(decision.valid);
  assert(!decision.should_start);
  assert(decision.has_time_manager_plan);
  assert(policy.last_plan.stop_reason == TIME_MANAGER_STOP_FUTURE_VALUE);

  // Conversely, ample value never overrides the indivisible completion gate.
  policy.clock.future_value_per_second = 0.0;
  policy.clock.future_reserve_seconds = 65.0;
  decision = endgame_admission_time_manager_callback(&request, &policy);
  assert(decision.valid);
  assert(!decision.should_start);
  assert(policy.last_plan.stop_reason == TIME_MANAGER_STOP_COMPLETION_RISK);

  // A valid p99 model cannot satisfy a policy that explicitly demands 100%.
  policy.clock.future_reserve_seconds = 20.0;
  policy.clock.minimum_completion_confidence = 1.0;
  decision = endgame_admission_time_manager_callback(&request, &policy);
  assert(decision.valid);
  assert(!decision.should_start);
  assert(policy.last_plan.stop_reason == TIME_MANAGER_STOP_COMPLETION_RISK);

  // There is no cross-turn allocation problem for an unbounded analysis.
  request.remaining_seconds = INFINITY;
  decision = endgame_admission_time_manager_callback(&request, &policy);
  assert(decision.valid);
  assert(decision.should_start);
  assert(!policy.has_last_plan);
}

static void test_endgame_admission_target5_value_prior(void) {
  EndgameDepthAdmissionRequest request = {
      .current =
          {
              .depth = 4,
              .best_move = 123,
              .value = 17,
          },
      .previous =
          {
              .depth = 3,
              .best_move = 123,
              .value = 17,
          },
      .has_previous = true,
      .next_depth = 5,
  };
  const EndgameDepthAdmissionDecision prediction = {.valid = true};
  assert(strcmp(ENDGAME_ADMISSION_TARGET5_VALUE_PRIOR_ID,
                "csw24-eg-target5-judge6-300-shadow-v1-20260729") == 0);
  assert_relative_near(
      endgame_admission_target5_regret_prior(&request, &prediction, NULL),
      0.000005649717514124294);

  request.current.best_move++;
  assert_relative_near(
      endgame_admission_target5_regret_prior(&request, &prediction, NULL),
      0.000026966292134831465);
  request.current.best_move = request.previous.best_move;
  request.current.value++;
  assert_relative_near(
      endgame_admission_target5_regret_prior(&request, &prediction, NULL),
      0.000026966292134831465);

  request.has_previous = false;
  assert_relative_near(
      endgame_admission_target5_regret_prior(&request, &prediction, NULL),
      0.000001);
  request.next_depth = 4;
  assert(isnan(
      endgame_admission_target5_regret_prior(&request, &prediction, NULL)));
}

static void test_endgame_future_depth5_reserve(void) {
  assert(strcmp(ENDGAME_FUTURE_DEPTH5_RESERVE_ID,
                "csw24-eg-future-depth5-observed-max-shadow-v0-20260729") == 0);
  assert(endgame_future_depth5_reserve_nodes(-1) == UINT64_MAX);
  assert(endgame_future_depth5_reserve_nodes(0) == 0);
  assert(endgame_future_depth5_reserve_nodes(3) == UINT64_C(10));
  assert(endgame_future_depth5_reserve_nodes(9) == UINT64_C(771026));
  assert(endgame_future_depth5_reserve_nodes(12) == UINT64_C(5819156));
  assert(endgame_future_depth5_reserve_nodes(13) == UINT64_C(13501513));
  assert(endgame_future_depth5_reserve_nodes(14) == UINT64_C(13501513));
  assert(endgame_future_depth5_reserve_nodes(15) == UINT64_C(13501513));
  uint64_t previous = 0;
  for (int tiles = 0; tiles <= 2 * RACK_SIZE; tiles++) {
    const uint64_t current = endgame_future_depth5_reserve_nodes(tiles);
    assert(current >= previous);
    previous = current;
  }
}

static void test_endgame_admission_tail_golden(void) {
  EndgameDepthAdmissionRequest request = {
      .current =
          {
              .depth = 4,
              .cumulative_nodes = 24404289,
              .elapsed_seconds = 31.211033,
              .root_moves_total = 7239,
              .ply2_moves_total = 267,
              .best_move = UINT64_C(22888316943),
              .value = 45,
          },
      .previous =
          {
              .depth = 3,
              .cumulative_nodes = 11726331,
              .elapsed_seconds = 15.976507,
              .root_moves_total = 7239,
              .ply2_moves_total = 201,
              .best_move = UINT64_C(22888316943),
              .value = 45,
          },
      .prior =
          {
              .depth = 2,
              .cumulative_nodes = 407301,
              .elapsed_seconds = 0.874518,
              .root_moves_total = 7239,
              .ply2_moves_total = 267,
              .best_move = UINT64_C(71782803443713),
              .value = 34,
          },
      .has_previous = true,
      .has_prior = true,
      .next_depth = 5,
      .requested_plies = 6,
      .workers = 10,
      .player_rack_tiles = 7,
      .opponent_rack_tiles = 6,
      .score_spread = -59,
  };
  const EndgameAdmissionPrediction prediction = endgame_admission_model_predict(
      endgame_admission_default_model(), &request);
  assert(prediction.valid);
  assert(prediction.boundary_protocol_matches);
  assert(prediction.expected_nodes == UINT64_C(63016717));
  assert(prediction.completion_bound_nodes == UINT64_C(1739169468));
  assert_relative_near(prediction.live_nodes_per_second, 832185.9176977347);
  assert_relative_near(prediction.expected_seconds, 75.72432436105795);
  assert_relative_near(prediction.completion_bound_seconds, 1462.9516735169304);

  request.remaining_seconds = INFINITY;
  EndgameAdmissionPolicy policy = {
      .model = endgame_admission_default_model(),
      .tt_fraction_of_mem = 0.05,
      .use_heuristics = true,
  };
  const EndgameDepthAdmissionDecision decision =
      endgame_admission_model_callback(&request, &policy);
  assert(decision.valid);
  assert(decision.safe_to_enforce);
  assert(decision.should_start);
}

static void test_endgame_admission_missing_history(void) {
  EndgameDepthAdmissionRequest request = {
      .current =
          {
              .depth = 3,
              .cumulative_nodes = 100000,
              .elapsed_seconds = 0.2,
              .root_moves_total = 250,
              .ply2_moves_total = 0,
          },
      .has_previous = false,
      .has_prior = false,
      .next_depth = 4,
      .requested_plies = 6,
      .workers = 10,
      .player_rack_tiles = 7,
      .opponent_rack_tiles = 6,
      .score_spread = -25,
  };
  double features[ENDGAME_ADMISSION_FEATURE_COUNT];
  assert(endgame_admission_build_features(&request, features));
  assert(features[ENDGAME_ADMISSION_FEATURE_LAST_INCREMENT_MISSING] == 1.0);
  assert(features[ENDGAME_ADMISSION_FEATURE_PRIOR_INCREMENT_MISSING] == 1.0);
  assert(features[ENDGAME_ADMISSION_FEATURE_BEST_MOVE_CHANGED] == 0.0);
  assert(features[ENDGAME_ADMISSION_FEATURE_SEARCH_VALUE_CHANGED] == 0.0);

  EndgameAdmissionPrediction prediction = endgame_admission_model_predict(
      endgame_admission_default_model(), &request);
  assert(prediction.valid);
  assert(prediction.expected_nodes == UINT64_C(80535));
  assert(prediction.completion_bound_nodes == UINT64_C(1644592));
  assert_relative_near(prediction.live_nodes_per_second, 500000.0);
  assert_relative_near(prediction.completion_bound_seconds, 1.9809670895109432);

  // A published but nonadjacent ABDADA boundary is missing history, not an
  // enormous two-ply increment.
  request.has_previous = true;
  request.previous.depth = 1;
  request.previous.cumulative_nodes = 1000;
  request.previous.elapsed_seconds = 0.01;
  assert(endgame_admission_build_features(&request, features));
  assert(features[ENDGAME_ADMISSION_FEATURE_LAST_INCREMENT_MISSING] == 1.0);

  request.next_depth = 6;
  prediction = endgame_admission_model_predict(
      endgame_admission_default_model(), &request);
  assert(!prediction.valid);
}

void test_endgame_admission_model(void) {
  test_endgame_admission_python_c_golden();
  test_endgame_admission_policy();
  test_endgame_admission_time_manager_bridge();
  test_endgame_admission_target5_value_prior();
  test_endgame_future_depth5_reserve();
  test_endgame_admission_tail_golden();
  test_endgame_admission_missing_history();
}
