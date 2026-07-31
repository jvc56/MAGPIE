#include "peg_time_manager.h"

#include <math.h>
#include <stddef.h>

enum {
  PEG_TIME_MANAGER_REFERENCE_WORKERS = 18,
  PEG_TIME_MANAGER_TAIL_POSITIONS = 320,
  PEG_TIME_MANAGER_HELDOUT_POSITIONS = 64,
};

static const double PEG_TIME_MANAGER_P99_COVERAGE_CONFIDENCE =
    0.9598891125131245; // 1 - 0.99^320

// Loaded-M5 least-squares conversion over 4,480 completed PEG boundaries.
// The fitted candidate coefficient was -0.0001801457 seconds and is clamped
// to zero: a negative cost is not meaningful for scheduling.
static const double PEG_TIME_MANAGER_REFERENCE_FIXED_SECONDS =
    0.6649604943560226;
static const double PEG_TIME_MANAGER_REFERENCE_SECONDS_PER_SCENARIO =
    4.490316923672115e-6;
static const double PEG_TIME_MANAGER_REFERENCE_SECONDS_PER_ENDGAME_NODE =
    5.162207041055367e-7;

#define PEG_WORK(node_count, scenario_count, candidate_count)                  \
  {                                                                            \
      .mode = ANALYSIS_MODE_PEG,                                               \
      .nodes = UINT64_C(node_count),                                           \
      .scenarios = UINT64_C(scenario_count),                                   \
      .candidates = UINT64_C(candidate_count),                                 \
  }

static const PegTimeManagerCalibration PEG_TIME_MANAGER_CALIBRATIONS[] = {
    {
        .boundary_kind = PEG_TIME_MANAGER_BOUNDARY_FIRST_TWO_2PLY,
        .bag_tiles = 1,
        .stage_index = 1,
        .fidelity_plies = 2,
        .reference_workers = PEG_TIME_MANAGER_REFERENCE_WORKERS,
        .candidates = 2,
        .required_completed_2ply_candidates = 0,
        .nested_enabled = false,
        .scenario_stride = 1,
        .parallel_wave_dispatch = true,
        .complete_positions = PEG_TIME_MANAGER_TAIL_POSITIONS,
        .censored_positions = 0,
        .median_work = PEG_WORK(62832, 14, 2),
        .empirical_p99_work = PEG_WORK(2306677, 16, 2),
        .empirical_max_work = PEG_WORK(3378394, 16, 2),
        .heldout_positions = PEG_TIME_MANAGER_HELDOUT_POSITIONS,
        .heldout_false_starts = 1,
        .target_completion_quantile = 0.99,
        .empirical_max_p99_coverage_confidence =
            PEG_TIME_MANAGER_P99_COVERAGE_CONFIDENCE,
        .reference_fixed_seconds_per_wave =
            PEG_TIME_MANAGER_REFERENCE_FIXED_SECONDS,
        .reference_seconds_per_scenario =
            PEG_TIME_MANAGER_REFERENCE_SECONDS_PER_SCENARIO,
        .reference_seconds_per_endgame_node =
            PEG_TIME_MANAGER_REFERENCE_SECONDS_PER_ENDGAME_NODE,
        .reference_seconds_per_candidate = 0.0,
    },
    {
        .boundary_kind = PEG_TIME_MANAGER_BOUNDARY_FIRST_TWO_2PLY,
        .bag_tiles = 2,
        .stage_index = 1,
        .fidelity_plies = 2,
        .reference_workers = PEG_TIME_MANAGER_REFERENCE_WORKERS,
        .candidates = 2,
        .required_completed_2ply_candidates = 0,
        .nested_enabled = false,
        .scenario_stride = 1,
        .parallel_wave_dispatch = true,
        .complete_positions = PEG_TIME_MANAGER_TAIL_POSITIONS,
        .censored_positions = 0,
        .median_work = PEG_WORK(155348, 58, 2),
        .empirical_p99_work = PEG_WORK(2790781, 144, 2),
        .empirical_max_work = PEG_WORK(5110942, 144, 2),
        .heldout_positions = PEG_TIME_MANAGER_HELDOUT_POSITIONS,
        .heldout_false_starts = 0,
        .target_completion_quantile = 0.99,
        .empirical_max_p99_coverage_confidence =
            PEG_TIME_MANAGER_P99_COVERAGE_CONFIDENCE,
        .reference_fixed_seconds_per_wave =
            PEG_TIME_MANAGER_REFERENCE_FIXED_SECONDS,
        .reference_seconds_per_scenario =
            PEG_TIME_MANAGER_REFERENCE_SECONDS_PER_SCENARIO,
        .reference_seconds_per_endgame_node =
            PEG_TIME_MANAGER_REFERENCE_SECONDS_PER_ENDGAME_NODE,
        .reference_seconds_per_candidate = 0.0,
    },
    {
        .boundary_kind = PEG_TIME_MANAGER_BOUNDARY_FIRST_TWO_2PLY,
        .bag_tiles = 3,
        .stage_index = 1,
        .fidelity_plies = 2,
        .reference_workers = PEG_TIME_MANAGER_REFERENCE_WORKERS,
        .candidates = 2,
        .required_completed_2ply_candidates = 0,
        .nested_enabled = false,
        .scenario_stride = 1,
        .parallel_wave_dispatch = true,
        .complete_positions = PEG_TIME_MANAGER_TAIL_POSITIONS,
        .censored_positions = 0,
        .median_work = PEG_WORK(212699, 266, 2),
        .empirical_p99_work = PEG_WORK(4571562, 1056, 2),
        .empirical_max_work = PEG_WORK(7399314, 1056, 2),
        .heldout_positions = PEG_TIME_MANAGER_HELDOUT_POSITIONS,
        .heldout_false_starts = 0,
        .target_completion_quantile = 0.99,
        .empirical_max_p99_coverage_confidence =
            PEG_TIME_MANAGER_P99_COVERAGE_CONFIDENCE,
        .reference_fixed_seconds_per_wave =
            PEG_TIME_MANAGER_REFERENCE_FIXED_SECONDS,
        .reference_seconds_per_scenario =
            PEG_TIME_MANAGER_REFERENCE_SECONDS_PER_SCENARIO,
        .reference_seconds_per_endgame_node =
            PEG_TIME_MANAGER_REFERENCE_SECONDS_PER_ENDGAME_NODE,
        .reference_seconds_per_candidate = 0.0,
    },
    {
        .boundary_kind = PEG_TIME_MANAGER_BOUNDARY_FIRST_TWO_2PLY,
        .bag_tiles = 4,
        .stage_index = 1,
        .fidelity_plies = 2,
        .reference_workers = PEG_TIME_MANAGER_REFERENCE_WORKERS,
        .candidates = 2,
        .required_completed_2ply_candidates = 0,
        .nested_enabled = false,
        .scenario_stride = 1,
        .parallel_wave_dispatch = true,
        .complete_positions = PEG_TIME_MANAGER_TAIL_POSITIONS,
        .censored_positions = 0,
        .median_work = PEG_WORK(186176, 1208, 2),
        .empirical_p99_work = PEG_WORK(11326478, 10944, 2),
        .empirical_max_work = PEG_WORK(29684225, 15840, 2),
        .heldout_positions = PEG_TIME_MANAGER_HELDOUT_POSITIONS,
        .heldout_false_starts = 0,
        .target_completion_quantile = 0.99,
        .empirical_max_p99_coverage_confidence =
            PEG_TIME_MANAGER_P99_COVERAGE_CONFIDENCE,
        .reference_fixed_seconds_per_wave =
            PEG_TIME_MANAGER_REFERENCE_FIXED_SECONDS,
        .reference_seconds_per_scenario =
            PEG_TIME_MANAGER_REFERENCE_SECONDS_PER_SCENARIO,
        .reference_seconds_per_endgame_node =
            PEG_TIME_MANAGER_REFERENCE_SECONDS_PER_ENDGAME_NODE,
        .reference_seconds_per_candidate = 0.0,
    },
    {
        .boundary_kind = PEG_TIME_MANAGER_BOUNDARY_FIRST_TWO_3PLY_AFTER_16,
        .bag_tiles = 2,
        .stage_index = 2,
        .fidelity_plies = 3,
        .reference_workers = PEG_TIME_MANAGER_REFERENCE_WORKERS,
        .candidates = 2,
        .required_completed_2ply_candidates = 16,
        .nested_enabled = false,
        .scenario_stride = 1,
        .parallel_wave_dispatch = true,
        .complete_positions = PEG_TIME_MANAGER_TAIL_POSITIONS,
        .censored_positions = 0,
        .median_work = PEG_WORK(2417603, 64, 2),
        .empirical_p99_work = PEG_WORK(173175320, 144, 2),
        .empirical_max_work = PEG_WORK(637317412, 144, 2),
        .heldout_positions = PEG_TIME_MANAGER_HELDOUT_POSITIONS,
        .heldout_false_starts = 0,
        .target_completion_quantile = 0.99,
        .empirical_max_p99_coverage_confidence =
            PEG_TIME_MANAGER_P99_COVERAGE_CONFIDENCE,
        .reference_fixed_seconds_per_wave =
            PEG_TIME_MANAGER_REFERENCE_FIXED_SECONDS,
        .reference_seconds_per_scenario =
            PEG_TIME_MANAGER_REFERENCE_SECONDS_PER_SCENARIO,
        .reference_seconds_per_endgame_node =
            PEG_TIME_MANAGER_REFERENCE_SECONDS_PER_ENDGAME_NODE,
        .reference_seconds_per_candidate = 0.0,
    },
    {
        .boundary_kind = PEG_TIME_MANAGER_BOUNDARY_FIRST_TWO_3PLY_AFTER_16,
        .bag_tiles = 3,
        .stage_index = 2,
        .fidelity_plies = 3,
        .reference_workers = PEG_TIME_MANAGER_REFERENCE_WORKERS,
        .candidates = 2,
        .required_completed_2ply_candidates = 16,
        .nested_enabled = false,
        .scenario_stride = 1,
        .parallel_wave_dispatch = true,
        .complete_positions = PEG_TIME_MANAGER_TAIL_POSITIONS,
        .censored_positions = 0,
        .median_work = PEG_WORK(1205910, 317, 2),
        .empirical_p99_work = PEG_WORK(130966268, 1056, 2),
        .empirical_max_work = PEG_WORK(452287912, 1440, 2),
        .heldout_positions = PEG_TIME_MANAGER_HELDOUT_POSITIONS,
        .heldout_false_starts = 0,
        .target_completion_quantile = 0.99,
        .empirical_max_p99_coverage_confidence =
            PEG_TIME_MANAGER_P99_COVERAGE_CONFIDENCE,
        .reference_fixed_seconds_per_wave =
            PEG_TIME_MANAGER_REFERENCE_FIXED_SECONDS,
        .reference_seconds_per_scenario =
            PEG_TIME_MANAGER_REFERENCE_SECONDS_PER_SCENARIO,
        .reference_seconds_per_endgame_node =
            PEG_TIME_MANAGER_REFERENCE_SECONDS_PER_ENDGAME_NODE,
        .reference_seconds_per_candidate = 0.0,
    },
};

#undef PEG_WORK

static const PegTimeManagerValueObservation
    PEG_TIME_MANAGER_2PLY_VALUE_OBSERVATIONS[] = {
        {
            .completed_candidates_before = 2,
            .completed_candidates_after = 4,
            .positions = 200,
            .disagreements = 30,
            .mean_incremental_scenarios = 752.82,
            .mean_incremental_endgame_nodes = 444790.575,
            .mean_incremental_candidates = 2.0,
            .mean_m5_seconds = 0.800331195,
            .observed_utility_gain = 0.00780361263,
            .observed_utility_ci_low = 0.0016982546941250011,
            .observed_utility_ci_high = 0.0166416741865,
            .observed_p_value = 0.0395960403959604,
        },
        {
            .completed_candidates_before = 4,
            .completed_candidates_after = 8,
            .positions = 200,
            .disagreements = 18,
            .mean_incremental_scenarios = 1609.445,
            .mean_incremental_endgame_nodes = 799676.55,
            .mean_incremental_candidates = 4.0,
            .mean_m5_seconds = 1.464680695,
            .observed_utility_gain = 0.00491625556,
            .observed_utility_ci_low = 0.0001726440356250018,
            .observed_utility_ci_high = 0.01143669556975,
            .observed_p_value = 0.07889211078892111,
        },
        {
            .completed_candidates_before = 8,
            .completed_candidates_after = 12,
            .positions = 200,
            .disagreements = 11,
            .mean_incremental_scenarios = 1592.44,
            .mean_incremental_endgame_nodes = 950357.005,
            .mean_incremental_candidates = 4.0,
            .mean_m5_seconds = 1.70513829,
            .observed_utility_gain = -0.003574570965,
            .observed_utility_ci_low = -0.008907451206625,
            .observed_utility_ci_high = 0.0000400062448749996,
            .observed_p_value = 0.11638836116388361,
        },
        {
            .completed_candidates_before = 12,
            .completed_candidates_after = 32,
            .positions = 200,
            .disagreements = 18,
            .mean_incremental_scenarios = 7977.79,
            .mean_incremental_endgame_nodes = 5060633.195,
            .mean_incremental_candidates = 20.0,
            .mean_m5_seconds = 9.20775075,
            .observed_utility_gain = 0.00410471667,
            .observed_utility_ci_low = 0.00005144166737499967,
            .observed_utility_ci_high = 0.010614266257124997,
            .observed_p_value = 0.08489151084891511,
        },
};

const PegTimeManagerCalibration *
peg_time_manager_default_calibration(PegTimeManagerBoundaryKind boundary_kind,
                                     int bag_tiles) {
  for (size_t index = 0; index < sizeof(PEG_TIME_MANAGER_CALIBRATIONS) /
                                     sizeof(PEG_TIME_MANAGER_CALIBRATIONS[0]);
       index++) {
    const PegTimeManagerCalibration *calibration =
        &PEG_TIME_MANAGER_CALIBRATIONS[index];
    if (calibration->boundary_kind == boundary_kind &&
        calibration->bag_tiles == bag_tiles) {
      return calibration;
    }
  }
  return NULL;
}

const PegTimeManagerValueObservation *
peg_time_manager_default_2ply_value_observation(
    int completed_candidates_before, int completed_candidates_after) {
  for (size_t index = 0;
       index < sizeof(PEG_TIME_MANAGER_2PLY_VALUE_OBSERVATIONS) /
                   sizeof(PEG_TIME_MANAGER_2PLY_VALUE_OBSERVATIONS[0]);
       index++) {
    const PegTimeManagerValueObservation *observation =
        &PEG_TIME_MANAGER_2PLY_VALUE_OBSERVATIONS[index];
    if (observation->completed_candidates_before ==
            completed_candidates_before &&
        observation->completed_candidates_after == completed_candidates_after) {
      return observation;
    }
  }
  return NULL;
}

static bool peg_time_manager_positive_finite(double value) {
  return isfinite(value) && value > 0.0;
}

static bool peg_time_manager_nonnegative_finite(double value) {
  return isfinite(value) && value >= 0.0;
}

bool peg_time_manager_reference_cost_models(
    const PegTimeManagerCalibration *calibration, double local_time_scale,
    double deadline_slowdown_multiplier,
    TimeManagerCostModel *expected_cost_model,
    TimeManagerCostModel *deadline_cost_model) {
  if (calibration == NULL || expected_cost_model == NULL ||
      deadline_cost_model == NULL ||
      !peg_time_manager_positive_finite(local_time_scale) ||
      !isfinite(deadline_slowdown_multiplier) ||
      deadline_slowdown_multiplier < 1.0 ||
      !peg_time_manager_nonnegative_finite(
          calibration->reference_fixed_seconds_per_wave) ||
      !peg_time_manager_nonnegative_finite(
          calibration->reference_seconds_per_scenario) ||
      !peg_time_manager_nonnegative_finite(
          calibration->reference_seconds_per_endgame_node) ||
      !peg_time_manager_nonnegative_finite(
          calibration->reference_seconds_per_candidate)) {
    return false;
  }

  *expected_cost_model = (TimeManagerCostModel){
      .peg_fixed_seconds_per_chunk =
          calibration->reference_fixed_seconds_per_wave * local_time_scale,
      .peg_seconds_per_scenario =
          calibration->reference_seconds_per_scenario * local_time_scale,
      .peg_seconds_per_endgame_node =
          calibration->reference_seconds_per_endgame_node * local_time_scale,
      .peg_seconds_per_candidate =
          calibration->reference_seconds_per_candidate * local_time_scale,
  };
  *deadline_cost_model = *expected_cost_model;
  deadline_cost_model->peg_fixed_seconds_per_chunk *=
      deadline_slowdown_multiplier;
  deadline_cost_model->peg_seconds_per_scenario *= deadline_slowdown_multiplier;
  deadline_cost_model->peg_seconds_per_endgame_node *=
      deadline_slowdown_multiplier;
  deadline_cost_model->peg_seconds_per_candidate *=
      deadline_slowdown_multiplier;
  return true;
}

static bool
peg_time_manager_request_matches(const PegTimeManagerCalibration *calibration,
                                 const PegTimeManagerRequest *request) {
  return request->boundary_kind == calibration->boundary_kind &&
         request->bag_tiles == calibration->bag_tiles &&
         request->stage_index == calibration->stage_index &&
         request->fidelity_plies == calibration->fidelity_plies &&
         request->workers > 0 &&
         request->candidates == calibration->candidates &&
         request->completed_2ply_candidates ==
             calibration->required_completed_2ply_candidates &&
         request->nested_enabled == calibration->nested_enabled &&
         request->scenario_stride == calibration->scenario_stride &&
         request->parallel_wave_dispatch == calibration->parallel_wave_dispatch;
}

PegTimeManagerDecision
peg_time_manager_plan_boundary(const PegTimeManagerPolicy *policy,
                               const PegTimeManagerRequest *request) {
  PegTimeManagerDecision decision = {0};
  if (policy == NULL || request == NULL ||
      request->boundary_kind == PEG_TIME_MANAGER_BOUNDARY_INVALID ||
      request->bag_tiles < 1 || request->bag_tiles > 4 ||
      !isfinite(request->elapsed_seconds) || request->elapsed_seconds < 0.0 ||
      isnan(request->remaining_seconds) || request->remaining_seconds < 0.0 ||
      (request->has_player_clock &&
       (!isfinite(request->player_clock_remaining_seconds) ||
        request->player_clock_remaining_seconds < 0.0))) {
    return decision;
  }

  const PegTimeManagerCalibration *calibration =
      peg_time_manager_default_calibration(request->boundary_kind,
                                           request->bag_tiles);
  if (calibration == NULL) {
    return decision;
  }
  decision.pricing_work = calibration->median_work;
  decision.empirical_p99_work = calibration->empirical_p99_work;
  decision.provisional_completion_bound_work = calibration->empirical_max_work;
  decision.configuration_matches =
      peg_time_manager_request_matches(calibration, request);
  if (!decision.configuration_matches) {
    return decision;
  }

  decision.pricing_seconds = time_manager_estimate_seconds(
      &policy->expected_cost_model, &decision.pricing_work);
  decision.empirical_p99_seconds = time_manager_estimate_seconds(
      &policy->deadline_cost_model, &decision.empirical_p99_work);
  decision.provisional_completion_bound_seconds = time_manager_estimate_seconds(
      &policy->deadline_cost_model,
      &decision.provisional_completion_bound_work);
  if (!isfinite(decision.pricing_seconds) || decision.pricing_seconds < 0.0 ||
      !isfinite(decision.empirical_p99_seconds) ||
      decision.empirical_p99_seconds < decision.pricing_seconds ||
      !isfinite(decision.provisional_completion_bound_seconds) ||
      decision.provisional_completion_bound_seconds <
          decision.empirical_p99_seconds) {
    return (PegTimeManagerDecision){0};
  }

  double expected_regret_reduction = policy->fixed_expected_regret_reduction;
  if (policy->regret_callback != NULL) {
    expected_regret_reduction = policy->regret_callback(
        request, calibration, policy->regret_callback_data);
  }
  if (!isfinite(expected_regret_reduction) || expected_regret_reduction < 0.0) {
    return (PegTimeManagerDecision){0};
  }
  decision.expected_regret_reduction = expected_regret_reduction;

  decision.has_provisional_completion_bound =
      calibration->censored_positions == 0 &&
      calibration->empirical_max_p99_coverage_confidence > 0.0;
  decision.completion_confidence =
      decision.has_provisional_completion_bound
          ? calibration->empirical_max_p99_coverage_confidence
          : NAN;

  TimeManagerClock clock = policy->clock;
  clock.remaining_seconds = request->has_player_clock
                                ? request->player_clock_remaining_seconds
                                : request->remaining_seconds;
  clock.committed_current_seconds = request->elapsed_seconds;
  const TimeManagerChunk chunk = {
      .boundary = TIME_MANAGER_BOUNDARY_PEG_WAVE,
      .work = decision.pricing_work,
      .has_completion_bound = decision.has_provisional_completion_bound,
      .completion_bound_work = decision.provisional_completion_bound_work,
      .completion_confidence = decision.completion_confidence,
      .expected_regret_reduction = expected_regret_reduction,
  };
  decision.plan = time_manager_plan(&clock, &policy->expected_cost_model,
                                    &policy->deadline_cost_model, &chunk, 1);
  if (!decision.plan.valid) {
    return (PegTimeManagerDecision){0};
  }

  const bool physically_fits = decision.has_provisional_completion_bound &&
                               decision.provisional_completion_bound_seconds <=
                                   request->remaining_seconds;
  decision.valid = true;
  // v1 records the correct work boundary but is still shadow-only. The
  // empirical maximum lacks the configured 99% evidence, no production safety
  // factor has been chosen, and the ordinary dispatcher launches larger
  // stages rather than this exact wave.
  decision.safe_to_enforce = false;
  decision.should_start = physically_fits && decision.plan.chunks_bought == 1;
  return decision;
}
