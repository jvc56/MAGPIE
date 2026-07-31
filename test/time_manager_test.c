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
  assert(plan.chunks_bought == 0);
  assert(plan.stop_reason == TIME_MANAGER_STOP_FUTURE_VALUE);
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

  // TimeManager plans against the total player clock, while the current
  // solver window remains a separate physical completion gate.
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
  test_time_manager_invalid_input();
  test_peg_time_manager_frozen_calibration();
  test_peg_time_manager_fails_closed_on_tail();
}
