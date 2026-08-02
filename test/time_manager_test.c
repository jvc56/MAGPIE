#include "time_manager_test.h"

#include "../src/impl/peg_time_manager.h"
#include "../src/impl/time_manager.h"
#include <assert.h>
#include <math.h>

static void assert_near(double actual, double expected) {
  assert(fabs(actual - expected) < 1.0e-9);
}

static const TimeManagerCostModel TEST_COST = {
    .sim_seconds_per_node = 1.0e-6,
    .endgame_seconds_per_node = 2.0e-6,
    .peg_seconds_per_scenario = 1.0e-4,
    .peg_seconds_per_endgame_node = 3.0e-6,
    .peg_seconds_per_candidate = 0.01,
};

static const TimeManagerCostModel TEST_DEADLINE_COST = {
    .sim_seconds_per_node = 1.25e-6,
    .endgame_seconds_per_node = 3.0e-6,
    .peg_seconds_per_scenario = 1.25e-4,
    .peg_seconds_per_endgame_node = 4.0e-6,
    .peg_seconds_per_candidate = 0.0125,
};

static void test_time_manager_cost_units(void) {
  TimeManagerWork work = {
      .mode = ANALYSIS_MODE_SIM,
      .nodes = 100000,
      .fixed_seconds = 0.02,
  };
  assert_near(time_manager_estimate_seconds(&TEST_COST, &work), 0.12);

  work = (TimeManagerWork){
      .mode = ANALYSIS_MODE_ENDGAME,
      .nodes = 250000,
  };
  assert_near(time_manager_estimate_seconds(&TEST_COST, &work), 0.5);

  work = (TimeManagerWork){
      .mode = ANALYSIS_MODE_PEG,
      .nodes = 100000,
      .scenarios = 200,
      .candidates = 4,
      .fixed_seconds = 0.01,
  };
  assert_near(time_manager_estimate_seconds(&TEST_COST, &work), 0.37);

  TimeManagerCostModel peg_fixed_cost = TEST_COST;
  peg_fixed_cost.peg_fixed_seconds_per_chunk = 0.02;
  assert_near(time_manager_estimate_seconds(&peg_fixed_cost, &work), 0.39);

  work.mode = ANALYSIS_MODE_PLAY_CHOOSER;
  assert(isnan(time_manager_estimate_seconds(&TEST_COST, &work)));
}

static void test_time_manager_deposit(void) {
  const TimeManagerClock clock = {
      .remaining_seconds = 100.0,
      .turns_remaining = 10,
      .safety_reserve_seconds = 1.0,
      .future_reserve_seconds = 0.9,
      .committed_current_seconds = 0.05,
      .future_value_per_second = 0.01,
      .minimum_completion_confidence = 0.99,
  };
  const TimeManagerChunk chunks[] = {
      {
          .boundary = TIME_MANAGER_BOUNDARY_SIM_CHECKPOINT,
          .work = {.mode = ANALYSIS_MODE_SIM, .nodes = 100000},
          .expected_regret_reduction = 0.01,
      },
      {
          .boundary = TIME_MANAGER_BOUNDARY_SIM_CHECKPOINT,
          .work = {.mode = ANALYSIS_MODE_SIM, .nodes = 500000},
          .expected_regret_reduction = 0.01,
      },
      {
          .boundary = TIME_MANAGER_BOUNDARY_SIM_CHECKPOINT,
          .work = {.mode = ANALYSIS_MODE_SIM, .nodes = 1000000},
          .expected_regret_reduction = 0.005,
      },
  };
  const TimeManagerPlan plan =
      time_manager_plan(&clock, &TEST_COST, &TEST_DEADLINE_COST, chunks, 3);
  assert(plan.valid);
  assert(plan.chunks_bought == 2);
  assert(plan.stop_reason == TIME_MANAGER_STOP_FUTURE_VALUE);
  assert_near(plan.planned_seconds, 0.65);
  assert_near(plan.expected_regret_reduction, 0.02);
  assert_near(plan.equal_slice_seconds, 9.9);
  assert_near(plan.deposit_seconds, 9.25);
  assert_near(plan.stopped_chunk_value_per_second, 0.005);
}

static void test_time_manager_withdrawal(void) {
  const TimeManagerClock clock = {
      .remaining_seconds = 100.0,
      .turns_remaining = 10,
      .safety_reserve_seconds = 1.0,
      .future_reserve_seconds = 0.9,
      .committed_current_seconds = 0.05,
      .future_value_per_second = 0.001,
      .minimum_completion_confidence = 0.99,
  };
  const TimeManagerChunk chunks[] = {
      {
          .boundary = TIME_MANAGER_BOUNDARY_ENDGAME_DEPTH,
          .work = {.mode = ANALYSIS_MODE_ENDGAME, .nodes = 5000000},
          .has_completion_bound = true,
          .completion_bound_work = {.mode = ANALYSIS_MODE_ENDGAME,
                                    .nodes = 6000000},
          .completion_confidence = 0.99,
          .expected_regret_reduction = 0.1,
      },
      {
          .boundary = TIME_MANAGER_BOUNDARY_ENDGAME_DEPTH,
          .work = {.mode = ANALYSIS_MODE_ENDGAME, .nodes = 2500000},
          .has_completion_bound = true,
          .completion_bound_work = {.mode = ANALYSIS_MODE_ENDGAME,
                                    .nodes = 3000000},
          .completion_confidence = 0.99,
          .expected_regret_reduction = 0.05,
      },
  };
  const TimeManagerPlan plan =
      time_manager_plan(&clock, &TEST_COST, &TEST_DEADLINE_COST, chunks, 2);
  assert(plan.valid);
  assert(plan.chunks_bought == 2);
  assert(plan.stop_reason == TIME_MANAGER_STOP_NO_MORE_CHUNKS);
  assert_near(plan.planned_seconds, 15.05);
  assert(plan.deposit_seconds < 0.0);
  assert_near(plan.deposit_seconds, -5.15);
}

static void test_time_manager_reserve_and_sequential_boundaries(void) {
  const TimeManagerClock short_clock = {
      .remaining_seconds = 2.0,
      .turns_remaining = 5,
      .safety_reserve_seconds = 0.5,
      .future_reserve_seconds = 0.8,
      .committed_current_seconds = 0.1,
      .future_value_per_second = 0.0,
      .minimum_completion_confidence = 0.99,
  };
  const TimeManagerChunk too_large = {
      .boundary = TIME_MANAGER_BOUNDARY_PEG_STAGE,
      .work =
          {
              .mode = ANALYSIS_MODE_PEG,
              .nodes = 200000,
              .scenarios = 100,
              .candidates = 1,
          },
      .has_completion_bound = true,
      .completion_bound_work =
          {
              .mode = ANALYSIS_MODE_PEG,
              .nodes = 250000,
              .scenarios = 120,
              .candidates = 1,
          },
      .completion_confidence = 0.99,
      .expected_regret_reduction = 1.0,
  };
  TimeManagerPlan plan = time_manager_plan(&short_clock, &TEST_COST,
                                           &TEST_DEADLINE_COST, &too_large, 1);
  assert(plan.valid);
  assert(plan.chunks_bought == 0);
  assert(plan.stop_reason == TIME_MANAGER_STOP_COMPLETION_RISK);
  assert_near(plan.maximum_current_seconds, 0.7);

  const TimeManagerClock value_clock = {
      .remaining_seconds = 10.0,
      .turns_remaining = 2,
      .future_value_per_second = 0.02,
      .minimum_completion_confidence = 0.99,
  };
  const TimeManagerChunk chunks[] = {
      {
          .boundary = TIME_MANAGER_BOUNDARY_PEG_CANDIDATE,
          .work = {.mode = ANALYSIS_MODE_PEG, .scenarios = 1000},
          .has_completion_bound = true,
          .completion_bound_work = {.mode = ANALYSIS_MODE_PEG,
                                    .scenarios = 1200},
          .completion_confidence = 0.99,
          .expected_regret_reduction = 0.001,
      },
      {
          .boundary = TIME_MANAGER_BOUNDARY_PEG_STAGE,
          .work = {.mode = ANALYSIS_MODE_PEG, .scenarios = 1000},
          .has_completion_bound = true,
          .completion_bound_work = {.mode = ANALYSIS_MODE_PEG,
                                    .scenarios = 1200},
          .completion_confidence = 0.99,
          .expected_regret_reduction = 1.0,
      },
  };
  plan = time_manager_plan(&value_clock, &TEST_COST, &TEST_DEADLINE_COST,
                           chunks, 2);
  assert(plan.valid);
  // The first boundary is weak in isolation, but it is required to reach the
  // valuable second boundary. Package-aware planning buys both.
  assert(plan.chunks_bought == 2);
  assert(plan.stop_reason == TIME_MANAGER_STOP_NO_MORE_CHUNKS);
  assert_near(plan.expected_regret_reduction, 1.001);
}

static void test_time_manager_completion_admission(void) {
  const TimeManagerClock clock = {
      .remaining_seconds = 2.0,
      .turns_remaining = 2,
      .safety_reserve_seconds = 0.1,
      .future_reserve_seconds = 0.2,
      .committed_current_seconds = 0.1,
      .future_value_per_second = 0.0,
      .minimum_completion_confidence = 0.99,
  };
  const TimeManagerChunk uncertain_depth = {
      .boundary = TIME_MANAGER_BOUNDARY_ENDGAME_DEPTH,
      .work = {.mode = ANALYSIS_MODE_ENDGAME, .nodes = 500000},
      .has_completion_bound = true,
      .completion_bound_work = {.mode = ANALYSIS_MODE_ENDGAME,
                                .nodes = 1000000},
      .completion_confidence = 0.99,
      .expected_regret_reduction = 1.0,
  };
  TimeManagerPlan plan = time_manager_plan(
      &clock, &TEST_COST, &TEST_DEADLINE_COST, &uncertain_depth, 1);
  assert(plan.valid);
  assert(plan.chunks_bought == 0);
  assert(plan.stop_reason == TIME_MANAGER_STOP_COMPLETION_RISK);
  assert_near(plan.maximum_current_seconds, 1.7);
  assert_near(plan.planned_seconds, 0.1);
  assert_near(plan.planned_completion_bound_seconds, 0.1);
  assert_near(plan.stopped_chunk_seconds, 1.0);
  assert_near(plan.stopped_chunk_completion_bound_seconds, 3.0);
  assert_near(plan.stopped_chunk_completion_confidence, 0.99);

  const TimeManagerClock ample_clock = {
      .remaining_seconds = 4.0,
      .turns_remaining = 2,
      .safety_reserve_seconds = 0.1,
      .future_reserve_seconds = 0.2,
      .committed_current_seconds = 0.1,
      .future_value_per_second = 0.0,
      .minimum_completion_confidence = 0.99,
  };
  plan = time_manager_plan(&ample_clock, &TEST_COST, &TEST_DEADLINE_COST,
                           &uncertain_depth, 1);
  assert(plan.valid);
  assert(plan.chunks_bought == 1);
  assert(plan.stop_reason == TIME_MANAGER_STOP_NO_MORE_CHUNKS);
  assert_near(plan.planned_seconds, 1.1);
  assert_near(plan.planned_completion_bound_seconds, 3.1);

  TimeManagerChunk invalid_bound = uncertain_depth;
  invalid_bound.completion_bound_work.nodes = 100000;
  plan = time_manager_plan(&ample_clock, &TEST_COST, &TEST_DEADLINE_COST,
                           &invalid_bound, 1);
  assert(!plan.valid);
  assert(plan.stop_reason == TIME_MANAGER_STOP_INVALID_INPUT);

  TimeManagerChunk missing_bound = uncertain_depth;
  missing_bound.has_completion_bound = false;
  plan = time_manager_plan(&ample_clock, &TEST_COST, &TEST_DEADLINE_COST,
                           &missing_bound, 1);
  assert(plan.valid);
  assert(plan.chunks_bought == 0);
  assert(plan.stop_reason == TIME_MANAGER_STOP_COMPLETION_RISK);
  assert(isnan(plan.stopped_chunk_completion_bound_seconds));
  assert(isnan(plan.stopped_chunk_completion_confidence));

  TimeManagerChunk low_confidence_bound = uncertain_depth;
  low_confidence_bound.completion_confidence = 0.95;
  plan = time_manager_plan(&ample_clock, &TEST_COST, &TEST_DEADLINE_COST,
                           &low_confidence_bound, 1);
  assert(plan.valid);
  assert(plan.chunks_bought == 0);
  assert(plan.stop_reason == TIME_MANAGER_STOP_COMPLETION_RISK);
  assert_near(plan.stopped_chunk_completion_confidence, 0.95);

  TimeManagerCostModel optimistic_deadline_cost = TEST_DEADLINE_COST;
  optimistic_deadline_cost.sim_seconds_per_node = 0.5e-6;
  const TimeManagerChunk deterministic_checkpoint = {
      .boundary = TIME_MANAGER_BOUNDARY_SIM_CHECKPOINT,
      .work = {.mode = ANALYSIS_MODE_SIM, .nodes = 500000},
      .expected_regret_reduction = 1.0,
  };
  plan = time_manager_plan(&ample_clock, &TEST_COST, &optimistic_deadline_cost,
                           &deterministic_checkpoint, 1);
  assert(!plan.valid);
  assert(plan.stop_reason == TIME_MANAGER_STOP_INVALID_INPUT);
}

static void test_time_manager_learned_value_to_go(void) {
  const TimeManagerValueKnot knots[] = {
      {.budget_seconds = 0.0, .expected_future_regret = 0.20},
      {.budget_seconds = 10.0, .expected_future_regret = 0.10},
      {.budget_seconds = 20.0, .expected_future_regret = 0.08},
  };
  TimeManagerValueCurve curve = {
      .knots = knots,
      .num_knots = sizeof(knots) / sizeof(knots[0]),
      .surrogate_allocation_gate_passed = true,
      .terminal_game_gate_passed = true,
  };
  assert(time_manager_value_curve_is_valid(&curve));
  assert_near(time_manager_value_curve_predict(&curve, 5.0), 0.15);
  assert_near(time_manager_value_curve_predict(&curve, 30.0), 0.08);
  assert_near(time_manager_value_curve_future_loss(15.0, 5.0, &curve),
              0.01);

  // The first five-second tranche is worth more than the exact suffix loss
  // from 16s -> 11s. Once it is bought, curvature makes the next identical
  // tranche much more expensive to the rest of the game (11s -> 6s), so the
  // planner stops. A constant lambda could not express this change.
  const TimeManagerClock clock = {
      .remaining_seconds = 16.0,
      .turns_remaining = 2,
      .future_value_per_second = 0.0,
      .future_loss_callback = time_manager_value_curve_future_loss,
      .future_loss_context = &curve,
      .minimum_completion_confidence = 0.99,
  };
  const TimeManagerChunk chunks[] = {
      {
          .boundary = TIME_MANAGER_BOUNDARY_SIM_CHECKPOINT,
          .work = {.mode = ANALYSIS_MODE_SIM, .nodes = 5000000},
          .expected_regret_reduction = 0.02,
      },
      {
          .boundary = TIME_MANAGER_BOUNDARY_SIM_CHECKPOINT,
          .work = {.mode = ANALYSIS_MODE_SIM, .nodes = 5000000},
          .expected_regret_reduction = 0.03,
      },
  };
  TimeManagerPlan plan = time_manager_plan(
      &clock, &TEST_COST, &TEST_DEADLINE_COST, chunks,
      sizeof(chunks) / sizeof(chunks[0]));
  assert(plan.valid);
  assert(plan.chunks_bought == 1);
  assert(plan.stop_reason == TIME_MANAGER_STOP_FUTURE_VALUE);
  assert_near(plan.expected_regret_reduction, 0.02);
  assert_near(plan.expected_future_regret_increase, 0.01);
  assert_near(plan.stopped_chunk_future_regret_increase, 0.042);

  // A curve that has not passed its terminal game-policy gate is not a
  // zero-valued future. It fails closed if anyone accidentally tries to use
  // an offline-surrogate-only model live.
  curve.terminal_game_gate_passed = false;
  assert(!time_manager_value_curve_is_valid(&curve));
  plan = time_manager_plan(&clock, &TEST_COST, &TEST_DEADLINE_COST, chunks, 1);
  assert(!plan.valid);
  assert(plan.stop_reason == TIME_MANAGER_STOP_INVALID_INPUT);
  curve.terminal_game_gate_passed = true;
  curve.surrogate_allocation_gate_passed = false;
  assert(!time_manager_value_curve_is_valid(&curve));
}

static void test_time_manager_invalid_input(void) {
  TimeManagerClock clock = {
      .remaining_seconds = 10.0,
      .turns_remaining = 0,
      .minimum_completion_confidence = 0.99,
  };
  TimeManagerPlan plan =
      time_manager_plan(&clock, &TEST_COST, &TEST_DEADLINE_COST, NULL, 0);
  assert(!plan.valid);
  assert(plan.stop_reason == TIME_MANAGER_STOP_INVALID_INPUT);

  clock.turns_remaining = 1;
  clock.minimum_completion_confidence = 0.0;
  plan = time_manager_plan(&clock, &TEST_COST, &TEST_DEADLINE_COST, NULL, 0);
  assert(!plan.valid);
  assert(plan.stop_reason == TIME_MANAGER_STOP_INVALID_INPUT);

  clock.minimum_completion_confidence = 0.99;
  const TimeManagerChunk invalid_chunk = {
      .work = {.mode = ANALYSIS_MODE_SIM, .nodes = 1},
      .expected_regret_reduction = -1.0,
  };
  plan = time_manager_plan(&clock, &TEST_COST, &TEST_DEADLINE_COST,
                           &invalid_chunk, 1);
  assert(!plan.valid);
  assert(plan.stop_reason == TIME_MANAGER_STOP_INVALID_INPUT);
}

static PegTimeManagerRequest
peg_time_manager_test_request(PegTimeManagerBoundaryKind boundary_kind,
                              int bag_tiles) {
  const bool is_three_ply =
      boundary_kind == PEG_TIME_MANAGER_BOUNDARY_FIRST_TWO_3PLY_AFTER_16;
  return (PegTimeManagerRequest){
      .boundary_kind = boundary_kind,
      .bag_tiles = bag_tiles,
      .stage_index = is_three_ply ? 2 : 1,
      .fidelity_plies = is_three_ply ? 3 : 2,
      // Tail work is portable; runtime cost calibration, not this field,
      // accounts for the local hardware/thread configuration.
      .workers = 10,
      .candidates = 2,
      .completed_2ply_candidates = is_three_ply ? 16 : 0,
      .nested_enabled = false,
      .scenario_stride = 1,
      .parallel_wave_dispatch = true,
      .completed_scenarios = 1000,
      .elapsed_seconds = 0.1,
      .remaining_seconds = 1000.0,
      .has_player_clock = true,
      .player_clock_remaining_seconds = 1000.0,
  };
}

static PegTimeManagerPolicy
peg_time_manager_test_policy(const PegTimeManagerCalibration *calibration) {
  PegTimeManagerPolicy policy = {
      .clock =
          {
              .remaining_seconds = 100.0,
              .turns_remaining = 5,
              .future_value_per_second = 0.0,
              .minimum_completion_confidence = 0.99,
          },
      .fixed_expected_regret_reduction = 0.1,
  };
  assert(peg_time_manager_reference_cost_models(
      calibration, /*local_time_scale=*/1.0,
      /*deadline_slowdown_multiplier=*/1.0, &policy.expected_cost_model,
      &policy.deadline_cost_model));
  return policy;
}

static void test_peg_time_manager_frozen_calibration(void) {
  const PegTimeManagerCalibration *bag1 = peg_time_manager_default_calibration(
      PEG_TIME_MANAGER_BOUNDARY_FIRST_TWO_2PLY, 1);
  const PegTimeManagerCalibration *bag2 = peg_time_manager_default_calibration(
      PEG_TIME_MANAGER_BOUNDARY_FIRST_TWO_2PLY, 2);
  const PegTimeManagerCalibration *bag4 = peg_time_manager_default_calibration(
      PEG_TIME_MANAGER_BOUNDARY_FIRST_TWO_2PLY, 4);
  const PegTimeManagerCalibration *deep2 = peg_time_manager_default_calibration(
      PEG_TIME_MANAGER_BOUNDARY_FIRST_TWO_3PLY_AFTER_16, 2);
  assert(bag1 != NULL);
  assert(bag2 != NULL);
  assert(bag4 != NULL);
  assert(deep2 != NULL);
  assert(peg_time_manager_default_calibration(
             PEG_TIME_MANAGER_BOUNDARY_FIRST_TWO_2PLY, 0) == NULL);
  assert(peg_time_manager_default_calibration(
             PEG_TIME_MANAGER_BOUNDARY_FIRST_TWO_3PLY_AFTER_16, 1) == NULL);

  assert(bag1->median_work.nodes == 62832);
  assert(bag1->median_work.scenarios == 14);
  assert(bag1->empirical_p99_work.nodes == 2306677);
  assert(bag1->empirical_max_work.nodes == 3378394);
  assert(bag1->complete_positions == 320);
  assert(bag1->censored_positions == 0);
  assert(bag1->heldout_positions == 64);
  assert(bag1->heldout_false_starts == 1);
  assert_near(bag1->empirical_max_p99_coverage_confidence, 0.9598891125131245);

  assert(bag2->median_work.nodes == 155348);
  assert(bag2->empirical_max_work.nodes == 5110942);
  assert(bag2->heldout_false_starts == 0);
  assert(bag4->empirical_max_work.scenarios == 15840);
  assert(bag4->empirical_max_work.nodes == 29684225);
  assert(deep2->required_completed_2ply_candidates == 16);
  assert(deep2->empirical_max_work.nodes == 637317412);

  const PegTimeManagerValueObservation *two_to_four =
      peg_time_manager_default_2ply_value_observation(2, 4);
  const PegTimeManagerValueObservation *eight_to_twelve =
      peg_time_manager_default_2ply_value_observation(8, 12);
  assert(two_to_four != NULL);
  assert(eight_to_twelve != NULL);
  assert(two_to_four->positions == 200);
  assert(two_to_four->disagreements == 30);
  assert_near(two_to_four->mean_incremental_endgame_nodes, 444790.575);
  assert_near(two_to_four->observed_utility_gain, 0.00780361263);
  assert(eight_to_twelve->observed_utility_gain < 0.0);
  assert(peg_time_manager_default_2ply_value_observation(2, 8) == NULL);

  TimeManagerCostModel expected;
  TimeManagerCostModel deadline;
  assert(peg_time_manager_reference_cost_models(
      bag4, /*local_time_scale=*/2.0,
      /*deadline_slowdown_multiplier=*/1.25, &expected, &deadline));
  assert_near(expected.peg_fixed_seconds_per_chunk, 2.0 * 0.6649604943560226);
  assert_near(expected.peg_seconds_per_scenario, 2.0 * 4.490316923672115e-6);
  assert_near(expected.peg_seconds_per_endgame_node,
              2.0 * 5.162207041055367e-7);
  assert_near(expected.peg_seconds_per_candidate, 0.0);
  assert_near(deadline.peg_fixed_seconds_per_chunk,
              2.0 * 1.25 * 0.6649604943560226);
  assert(!peg_time_manager_reference_cost_models(bag4, 0.0, 1.25, &expected,
                                                 &deadline));

  // The protected PEG forecast pays one fixed boundary cost for the first
  // pair and another for every later single-candidate admission. Only the
  // pair's portable variable work is halved for those later candidates.
  TimeManagerCostModel forecast_cost = TEST_COST;
  forecast_cost.peg_fixed_seconds_per_chunk = 0.2;
  TimeManagerWork single_candidate_work = bag1->median_work;
  single_candidate_work.nodes = (single_candidate_work.nodes + 1) / 2;
  single_candidate_work.scenarios = (single_candidate_work.scenarios + 1) / 2;
  single_candidate_work.candidates = 1;
  single_candidate_work.fixed_seconds *= 0.5;
  const double expected_minimum_seconds =
      time_manager_estimate_seconds(&forecast_cost, &bag1->median_work) +
      6.0 *
          time_manager_estimate_seconds(&forecast_cost, &single_candidate_work);
  assert_near(
      peg_time_manager_estimate_minimum_2ply_seconds(bag1, &forecast_cost, 8),
      expected_minimum_seconds);
  assert(isnan(
      peg_time_manager_estimate_minimum_2ply_seconds(NULL, &forecast_cost, 8)));
  assert(isnan(
      peg_time_manager_estimate_minimum_2ply_seconds(bag1, &forecast_cost, 1)));
}

static void test_peg_time_manager_complete_stage(void) {
  const PegTimeManagerStageCalibration *cold =
      peg_time_manager_default_stage_calibration(1, 1, 2);
  const PegTimeManagerStageCalibration *deeper =
      peg_time_manager_default_stage_calibration(1, 2, 3);
  assert(cold != NULL);
  assert(deeper != NULL);
  assert(cold->maximum_candidates == 32);
  assert(cold->source_positions == 28);
  assert(cold->heldout_positions == 0);
  assert(!cold->heldout_gate_passed);
  assert_near(cold->target_completion_quantile, 0.95);
  assert_near(cold->source_quantile_evidence, 0.7621731147);
  assert_near(cold->expected_nodes_per_scenario, 5200.0);
  assert_near(cold->completion_nodes_per_scenario, 50000.0);
  assert(cold->completion_absolute_nodes == UINT64_C(11000000));
  assert_near(cold->deadline_cost_multiplier, 5.0);
  assert_near(cold->warm_deadline_cost_multiplier, 2.0);
  assert(deeper->previous_fidelity_plies == 2);
  assert_near(deeper->expected_previous_node_multiplier, 6.0);
  assert_near(deeper->completion_previous_node_multiplier, 16.0);
  assert_near(deeper->warm_deadline_cost_multiplier, 1.0);
  assert(peg_time_manager_default_stage_calibration(1, 4, 5) == NULL);

  const PegTimeManagerCalibration *bag1 = peg_time_manager_default_calibration(
      PEG_TIME_MANAGER_BOUNDARY_FIRST_TWO_2PLY, 1);
  PegTimeManagerPolicy policy = peg_time_manager_test_policy(bag1);
  policy.clock.minimum_completion_confidence = 0.95;
  policy.clock.remaining_seconds = 1000.0;
  policy.completion_bound_multiplier = 1.5;
  policy.allow_provisional_enforcement = true;
  policy.use_complete_stage_admission = true;
  PegTimeManagerStageRequest request = {
      .bag_tiles = 1,
      .stage_index = 1,
      .fidelity_plies = 2,
      .previous_fidelity_plies = 0,
      .workers = 10,
      .candidates = 32,
      .scenario_stride = 1,
      .parallel_stage_dispatch = true,
      .stage_scenarios = 224,
      .previous_stage_scenarios = 224,
      .completed_scenarios = 5000,
      .elapsed_seconds = 0.1,
      .remaining_seconds = 1000.0,
      .has_player_clock = true,
      .player_clock_remaining_seconds = 1000.0,
  };
  PegTimeManagerDecision decision =
      peg_time_manager_plan_stage(&policy, &request);
  assert(decision.valid);
  assert(decision.configuration_matches);
  assert(decision.is_complete_stage);
  assert(!decision.uses_post_wave_tail_proxy);
  assert(!decision.heldout_gate_passed);
  assert(!decision.safe_to_enforce);
  assert(decision.should_start);
  assert(decision.plan.chunks_bought == 1);
  assert(decision.pricing_work.scenarios == 224);
  assert(decision.pricing_work.candidates == 32);
  assert(decision.pricing_work.nodes == UINT64_C(1164800));
  assert(decision.empirical_p99_work.nodes == UINT64_C(11000000));
  assert(decision.provisional_completion_bound_work.nodes ==
         UINT64_C(16500000));

  // The complete-depth contract may narrow the survivor prefix before launch,
  // but never publishes a partially evaluated prefix. Exact scenario sums
  // make the same frozen envelope valid at the ordinary 16/8 halving points.
  request.candidates = 16;
  request.stage_scenarios = 112;
  request.previous_stage_scenarios = 112;
  decision = peg_time_manager_plan_stage(&policy, &request);
  assert(decision.valid);
  assert(decision.pricing_work.candidates == 16);
  assert(decision.provisional_completion_bound_work.nodes == UINT64_C(8400000));
  request.candidates = 8;
  request.stage_scenarios = 56;
  request.previous_stage_scenarios = 56;
  decision = peg_time_manager_plan_stage(&policy, &request);
  assert(decision.valid);
  assert(decision.pricing_work.candidates == 8);
  assert(decision.provisional_completion_bound_work.nodes == UINT64_C(4200000));
  request.candidates = 32;
  request.stage_scenarios = 224;
  request.previous_stage_scenarios = 224;

  // A physical solver window remains an independent gate even when the game
  // clock could buy the depth. No part of the stage should launch.
  request.remaining_seconds = 1.0;
  decision = peg_time_manager_plan_stage(&policy, &request);
  assert(decision.valid);
  assert(decision.plan.chunks_bought == 1);
  assert(!decision.should_start);

  // At deeper boundaries, price the actual next-depth survivors from their
  // previous-depth work; a cold scenario floor protects unusually cheap prior
  // solves. Both coordinates are calculated for the whole 16-candidate stage.
  request = (PegTimeManagerStageRequest){
      .bag_tiles = 1,
      .stage_index = 2,
      .fidelity_plies = 3,
      .previous_fidelity_plies = 2,
      .workers = 10,
      .candidates = 16,
      .scenario_stride = 1,
      .parallel_stage_dispatch = true,
      .stage_scenarios = 112,
      .previous_stage_scenarios = 112,
      .previous_stage_endgame_nodes = 1000000,
      .completed_scenarios = 5224,
      .completed_endgame_nodes = 1000000,
      .elapsed_seconds = 2.0,
      .remaining_seconds = 1000.0,
      .has_player_clock = true,
      .player_clock_remaining_seconds = 1000.0,
  };
  decision = peg_time_manager_plan_stage(&policy, &request);
  assert(decision.valid);
  assert(decision.pricing_work.nodes == UINT64_C(6000000));
  assert(decision.empirical_p99_work.nodes == UINT64_C(16000000));
  assert(decision.provisional_completion_bound_work.nodes ==
         UINT64_C(24000000));

  request.previous_stage_scenarios--;
  decision = peg_time_manager_plan_stage(&policy, &request);
  assert(!decision.valid);
  assert(!decision.configuration_matches);

  // Unsupported depths and non-barrier dispatches fail closed rather than
  // borrowing a shallower or candidate-level tail.
  request.previous_stage_scenarios = request.stage_scenarios;
  request.stage_index = 4;
  request.fidelity_plies = 5;
  request.previous_fidelity_plies = 4;
  decision = peg_time_manager_plan_stage(&policy, &request);
  assert(!decision.valid);
  request.stage_index = 1;
  request.fidelity_plies = 2;
  request.previous_fidelity_plies = 0;
  request.previous_stage_endgame_nodes = 0;
  request.previous_stage_scenarios = request.stage_scenarios;
  request.parallel_stage_dispatch = false;
  decision = peg_time_manager_plan_stage(&policy, &request);
  assert(!decision.valid);

  // A slow prefix raises even an as-yet-unobserved endgame-node rate. A fast
  // node-free prefix may trim fixed/scenario overhead, but cannot make the
  // dominant cold node tail look four times faster.
  request.parallel_stage_dispatch = true;
  request.stage_scenarios = 224;
  request.previous_stage_scenarios = 224;
  request.candidates = 32;
  request.completed_scenarios = 5000;
  request.completed_endgame_nodes = 0;
  request.elapsed_seconds = 100.0;
  request.remaining_seconds = 1000.0;
  policy.use_live_cost_scale = true;
  const PegTimeManagerDecision slow =
      peg_time_manager_plan_stage(&policy, &request);
  assert(slow.valid);
  assert_near(slow.live_cost_scale, 4.0);
  policy.use_live_cost_scale = false;
  const PegTimeManagerDecision cold_decision =
      peg_time_manager_plan_stage(&policy, &request);
  assert(cold_decision.valid);
  policy.has_runtime_endgame_rate = true;
  const PegTimeManagerDecision warm_decision =
      peg_time_manager_plan_stage(&policy, &request);
  assert(warm_decision.valid);
  assert_near(warm_decision.provisional_completion_bound_seconds,
              cold_decision.provisional_completion_bound_seconds * 2.0 / 5.0);
  policy.has_runtime_endgame_rate = false;
  assert(slow.provisional_completion_bound_seconds >
         3.9 * cold_decision.provisional_completion_bound_seconds);
  policy.use_live_cost_scale = true;
  request.elapsed_seconds = 0.01;
  const PegTimeManagerDecision fast =
      peg_time_manager_plan_stage(&policy, &request);
  assert(fast.valid);
  assert_near(fast.live_cost_scale, 0.25);
  assert(fast.provisional_completion_bound_seconds >
         0.95 * cold_decision.provisional_completion_bound_seconds);

  PegTimeManagerRuntimeRate runtime_rate = {0};
  assert(peg_time_manager_runtime_rate_update(
      &runtime_rate, /*workers=*/10, /*stage_seconds=*/20.0,
      /*stage_endgame_nodes=*/UINT64_C(10000000)));
  assert(runtime_rate.observations == 1);
  TimeManagerCostModel runtime_expected = policy.expected_cost_model;
  TimeManagerCostModel runtime_deadline = policy.deadline_cost_model;
  assert(peg_time_manager_runtime_rate_apply(
      &runtime_rate, /*workers=*/5, /*deadline_safety_multiplier=*/1.5,
      &runtime_expected, &runtime_deadline));
  // 20s / 10M nodes * 10 source workers / 5 target workers.
  assert_near(runtime_expected.peg_seconds_per_endgame_node, 4.0e-6);
  assert_near(runtime_deadline.peg_seconds_per_endgame_node, 6.0e-6);
}

static void test_peg_time_manager_fails_closed_on_tail(void) {
  const PegTimeManagerCalibration *bag1 = peg_time_manager_default_calibration(
      PEG_TIME_MANAGER_BOUNDARY_FIRST_TWO_2PLY, 1);
  PegTimeManagerPolicy policy = peg_time_manager_test_policy(bag1);
  PegTimeManagerRequest request = peg_time_manager_test_request(
      PEG_TIME_MANAGER_BOUNDARY_FIRST_TWO_2PLY, 1);

  PegTimeManagerDecision decision =
      peg_time_manager_plan_boundary(&policy, &request);
  assert(decision.valid);
  assert(decision.configuration_matches);
  assert(decision.has_provisional_completion_bound);
  assert(!decision.safe_to_enforce);
  assert(!decision.should_start);
  assert(decision.plan.valid);
  assert(decision.plan.stop_reason == TIME_MANAGER_STOP_COMPLETION_RISK);
  assert_near(decision.completion_confidence,
              bag1->empirical_max_p99_coverage_confidence);

  // Lowering the policy threshold can expose the provisional shadow
  // recommendation, but never makes the v1 artifact safe to enforce.
  policy.clock.minimum_completion_confidence = 0.95;
  decision = peg_time_manager_plan_boundary(&policy, &request);
  assert(decision.valid);
  assert(decision.plan.chunks_bought == 1);
  assert(decision.should_start);
  assert(!decision.safe_to_enforce);

  // The live experimental policy must opt in explicitly. Its safety
  // multiplier enlarges only the deadline envelope; expected work/value
  // pricing stays at the measured median.
  policy.allow_provisional_enforcement = true;
  policy.completion_bound_multiplier = 1.5;
  decision = peg_time_manager_plan_boundary(&policy, &request);
  assert(decision.valid);
  assert(decision.safe_to_enforce);
  assert(decision.should_start);
  assert(decision.pricing_work.nodes == bag1->median_work.nodes);
  assert(decision.provisional_completion_bound_work.nodes == UINT64_C(5067591));

  // Later candidates are admitted singly, but conservatively retain the
  // whole first-two empirical maximum as a visible proxy tail.
  request.boundary_kind = PEG_TIME_MANAGER_BOUNDARY_NEXT_2PLY_CANDIDATE;
  request.candidates = 1;
  request.completed_2ply_candidates = 2;
  request.parallel_wave_dispatch = false;
  policy.regret_callback = peg_time_manager_default_regret_reduction;
  decision = peg_time_manager_plan_boundary(&policy, &request);
  assert(decision.valid);
  assert(decision.configuration_matches);
  assert(decision.uses_post_wave_tail_proxy);
  assert(decision.safe_to_enforce);
  assert(decision.pricing_work.nodes == UINT64_C(31416));
  assert(decision.pricing_work.candidates == 1);
  assert(decision.provisional_completion_bound_work.nodes == UINT64_C(5067591));
  assert(decision.provisional_completion_bound_work.candidates == 1);
  assert_near(decision.expected_regret_reduction, 0.00780361263 / 2.0);

  request.completed_2ply_candidates = 4;
  decision = peg_time_manager_plan_boundary(&policy, &request);
  assert_near(decision.expected_regret_reduction, 0.00491625556 / 4.0);
  request.completed_2ply_candidates = 8;
  decision = peg_time_manager_plan_boundary(&policy, &request);
  assert_near(decision.expected_regret_reduction, 0.000530145705 / 24.0);

  // Regression for pair 45 of the 2026-07-31 match. The completed bag-3
  // prefix was fast but searched zero endgame nodes. It therefore may rescale
  // scenario/fixed overhead, but must not use that observation to make the
  // unobserved endgame-node tail look faster. With the actual prefix work and
  // remaining PEG window, the next candidate must be refused.
  const PegTimeManagerCalibration *bag3 = peg_time_manager_default_calibration(
      PEG_TIME_MANAGER_BOUNDARY_FIRST_TWO_2PLY, 3);
  PegTimeManagerPolicy pair45_policy = {
      .clock =
          {
              .remaining_seconds = 34.572579,
              .turns_remaining = 2,
              .minimum_completion_confidence = 0.95,
          },
      .fixed_expected_regret_reduction = 0.01,
      .completion_bound_multiplier = 1.5,
      .allow_provisional_enforcement = true,
      .use_live_cost_scale = true,
  };
  assert(peg_time_manager_reference_cost_models(
      bag3, /*local_time_scale=*/1.8,
      /*deadline_slowdown_multiplier=*/1.5, &pair45_policy.expected_cost_model,
      &pair45_policy.deadline_cost_model));
  PegTimeManagerRequest pair45_request = {
      .boundary_kind = PEG_TIME_MANAGER_BOUNDARY_NEXT_2PLY_CANDIDATE,
      .bag_tiles = 3,
      .stage_index = 1,
      .fidelity_plies = 2,
      .workers = 10,
      .candidates = 1,
      .completed_2ply_candidates = 2,
      .nested_enabled = false,
      .scenario_stride = 1,
      .parallel_wave_dispatch = false,
      .completed_scenarios = 49276,
      .completed_endgame_nodes = 0,
      .elapsed_seconds = 0.691038,
      .remaining_seconds = 15.007188,
      .has_player_clock = true,
      .player_clock_remaining_seconds = 33.881541,
  };
  decision = peg_time_manager_plan_boundary(&pair45_policy, &pair45_request);
  assert(decision.valid);
  assert(decision.live_cost_scale < 0.5);
  assert(decision.provisional_completion_bound_seconds >
         pair45_request.remaining_seconds);
  assert(!decision.should_start);

  // The completed greedy prefix can rescale the hardware conversion before
  // the first refinement admission; the single-prefix estimate is clamped.
  policy.use_live_cost_scale = true;
  request.elapsed_seconds = 100.0;
  decision = peg_time_manager_plan_boundary(&policy, &request);
  assert(decision.valid);
  assert_near(decision.live_cost_scale, 4.0);

  // TimeManager plans against the total player clock, while the current
  // solver window remains a separate physical completion gate.
  request = peg_time_manager_test_request(
      PEG_TIME_MANAGER_BOUNDARY_FIRST_TWO_2PLY, 1);
  policy.regret_callback = NULL;
  policy.use_live_cost_scale = false;
  request.remaining_seconds = 1.0;
  decision = peg_time_manager_plan_boundary(&policy, &request);
  assert(decision.valid);
  assert(decision.plan.chunks_bought == 1);
  assert(!decision.should_start);

  // The deeper first-two wave is calibrated only for bags 2--3 and requires
  // the completed 16-candidate 2-ply boundary.
  const PegTimeManagerCalibration *deep2 = peg_time_manager_default_calibration(
      PEG_TIME_MANAGER_BOUNDARY_FIRST_TWO_3PLY_AFTER_16, 2);
  policy = peg_time_manager_test_policy(deep2);
  policy.clock.minimum_completion_confidence = 0.95;
  policy.clock.remaining_seconds = 1000.0;
  request = peg_time_manager_test_request(
      PEG_TIME_MANAGER_BOUNDARY_FIRST_TWO_3PLY_AFTER_16, 2);
  decision = peg_time_manager_plan_boundary(&policy, &request);
  assert(decision.valid);
  assert(decision.has_provisional_completion_bound);
  assert(decision.plan.chunks_bought == 1);
  assert(decision.should_start);
  assert(!decision.safe_to_enforce);

  request.completed_2ply_candidates = 8;
  decision = peg_time_manager_plan_boundary(&policy, &request);
  assert(!decision.valid);
  assert(!decision.configuration_matches);

  request = peg_time_manager_test_request(
      PEG_TIME_MANAGER_BOUNDARY_FIRST_TWO_2PLY, 2);
  request.candidates = 32;
  decision = peg_time_manager_plan_boundary(&policy, &request);
  assert(!decision.valid);
  assert(!decision.configuration_matches);
}

void test_time_manager(void) {
  test_time_manager_cost_units();
  test_time_manager_deposit();
  test_time_manager_withdrawal();
  test_time_manager_reserve_and_sequential_boundaries();
  test_time_manager_completion_admission();
  test_time_manager_learned_value_to_go();
  test_time_manager_invalid_input();
  test_peg_time_manager_frozen_calibration();
  test_peg_time_manager_complete_stage();
  test_peg_time_manager_fails_closed_on_tail();
}
