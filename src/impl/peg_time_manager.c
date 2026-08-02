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

// Direct-4 panel mean for full 32 candidates minus fixed 8, divided over the
// 24 extra candidates. This is deliberately a small rare-rescue prior: the
// raw 8->12 point estimate was negative while 12->32 was positive, so forcing
// the finite-sample curve to be monotone would overstate what candidate 9 is
// worth.
static const double PEG_TIME_MANAGER_POST8_VALUE_PER_CANDIDATE =
    0.000530145705 / 24.0;

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

// Complete-stage v6 is intentionally a shadow artifact. The cold 2-ply rates
// began as direct whole-stage ratios from the development panel, rounded
// outward. The first prospective expansion exposed a bag-3 tail at roughly
// 9,821 nodes/scenario, versus v1's 3,450 after its global safety multiplier.
// V2 folded that failed panel into training and rounded the bag-3 envelope out
// to 12,000 before the same multiplier. It had to pass a new untouched replay;
// the failed v1 panel is no longer eligible as validation evidence.
//
// V2's first prospective multi-depth replay then exposed a different error:
// after a runtime NPS sample existed, it removed the bag-specific wall/node
// residual. A bag-1 stage used 7.80M nodes (well below its 16.5M-node bound)
// but took 44.43s versus a 31.99s wall bound. Hardware throughput and
// position-specific cost per counted node are distinct effects. V3 therefore
// raises the persistent deadline-rate safety from 1.0 to the outward-rounded
// 1.75 that first covers the failed sequence. A fresh v3 panel then showed
// that a single warm node rate was still insufficient: bag-2 took 99.47s
// against a 43.34s bound, and bag-3 reached only 25/32 candidates in 178.71s
// against an 87.63s bound. V4 keeps the portable warm rate but adds a smaller
// bag-conditioned residual fitted to all failed prospective sequences. The
// outward-rounded warm residuals are 2.0x/3.0x/2.5x/1.25x for bags 1..4.
// Deeper stages retain 1.0x because their immediately preceding stage gives
// position-local work evidence. The bag-1 value is widened again to 2.0x
// because the strict replay of its known 44.43s tail exceeded the initial
// 1.5x proposal's 40.39s bound. All failed panels are training evidence only;
// every live gate remains closed pending a new untouched replay. V4's first
// untouched expansion was safe but admitted only 3/14 2-ply opportunities,
// 5/13 3-ply opportunities, and 3/12 4-ply opportunities. V5 therefore keeps
// the same conservative work envelope but, before launching anything, tests
// the ordinary halving prefixes and selects the largest of 32/16/8 which can
// finish. Its first prefix-8 panel exposed a runtime-observation bookkeeping
// bug rather than an envelope miss: a later zero-node depth erased a valid
// shallower positive-node sample. V6 preserves the deepest positive-node
// stage for the next-call hardware rate. The chosen prefix remains one
// indivisible publication boundary.
//
// The earlier attempt scaled the empirical maximum of
// a *two-candidate* wave across 32 candidates; it admitted only 2/20
// development stages and therefore was safe but operationally useless. The
// model below predicts the unit actually dispatched: every survivor at the
// next depth. Bag-specific deadline multipliers cover the largest observed
// whole-stage wall/work residual after the normal deadline and completion
// multipliers are applied.
//
// For the next two depths, the same survivors' previous-depth node count is
// available. A 6x expected / 16x completion growth envelope covers the pilot
// 2->3 observations while retaining the cold per-scenario floor. It remains
// unapproved for enforcement until a prospective replay populates the heldout
// fields below. Depths 5 and 6 deliberately have no entry and fail closed.
#define PEG_STAGE_CAL(bag, stage, fidelity, previous, maximum, positions,      \
                      evidence, expected_rate, bound_rate, absolute_nodes,     \
                      expected_growth, bound_growth, deadline_multiplier,      \
                      warm_deadline_multiplier, regret)                        \
  {                                                                            \
      .bag_tiles = (bag),                                                      \
      .stage_index = (stage),                                                  \
      .fidelity_plies = (fidelity),                                            \
      .previous_fidelity_plies = (previous),                                   \
      .maximum_candidates = (maximum),                                         \
      .source_positions = (positions),                                         \
      .heldout_positions = 0,                                                  \
      .heldout_false_starts = 0,                                               \
      .target_completion_quantile = 0.95,                                      \
      .source_quantile_evidence = (evidence),                                  \
      .expected_nodes_per_scenario = (expected_rate),                          \
      .completion_nodes_per_scenario = (bound_rate),                           \
      .completion_absolute_nodes = UINT64_C(absolute_nodes),                   \
      .expected_previous_node_multiplier = (expected_growth),                  \
      .completion_previous_node_multiplier = (bound_growth),                   \
      .deadline_cost_multiplier = (deadline_multiplier),                       \
      .warm_deadline_cost_multiplier = (warm_deadline_multiplier),             \
      .expected_regret_reduction = (regret),                                   \
      .provisional_completion_confidence = 0.95,                               \
      .heldout_gate_passed = false,                                            \
  }

static const PegTimeManagerStageCalibration
    PEG_TIME_MANAGER_STAGE_CALIBRATIONS[] = {
        // The untouched live gate remains mandatory before any
        // heldout_gate_passed field may change.
        PEG_STAGE_CAL(1, 1, 2, 0, 32, 28, 0.7621731147, 5200.0, 50000.0,
                      11000000, 0.0, 0.0, 5.0, 2.0, 0.0903),
        PEG_STAGE_CAL(2, 1, 2, 0, 32, 28, 0.7621731147, 1900.0, 30000.0,
                      19000000, 0.0, 0.0, 2.0, 3.0, 0.0903),
        // The failed v1 panel is explicitly reclassified as training.
        PEG_STAGE_CAL(3, 1, 2, 0, 32, 28, 0.7621731147, 425.0, 12000.0,
                      37000000, 0.0, 0.0, 1.35, 2.5, 0.0903),
        PEG_STAGE_CAL(4, 1, 2, 0, 32, 26, 0.7364799055, 190.0, 3000.0, 50000000,
                      0.0, 0.0, 1.0, 1.25, 0.0903),
        // 1 - 0.95^40 = 87.15%; these depth-growth entries are also shadow
        // until the same prospective gate is met.
        PEG_STAGE_CAL(1, 2, 3, 2, 16, 40, 0.8714878434, 5200.0, 30000.0, 0, 6.0,
                      16.0, 1.0, 1.0, 0.000530145705),
        PEG_STAGE_CAL(2, 2, 3, 2, 16, 40, 0.8714878434, 1900.0, 22000.0, 0, 6.0,
                      16.0, 1.0, 1.0, 0.000530145705),
        PEG_STAGE_CAL(3, 2, 3, 2, 16, 40, 0.8714878434, 425.0, 12000.0, 0, 6.0,
                      16.0, 1.0, 1.0, 0.000530145705),
        PEG_STAGE_CAL(4, 2, 3, 2, 16, 40, 0.8714878434, 190.0, 3000.0, 0, 6.0,
                      16.0, 1.0, 1.0, 0.000530145705),
        PEG_STAGE_CAL(1, 3, 4, 3, 8, 40, 0.8714878434, 5200.0, 30000.0, 0, 6.0,
                      16.0, 1.0, 1.0, 0.00164917222),
        PEG_STAGE_CAL(2, 3, 4, 3, 8, 40, 0.8714878434, 1900.0, 22000.0, 0, 6.0,
                      16.0, 1.0, 1.0, 0.00164917222),
        PEG_STAGE_CAL(3, 3, 4, 3, 8, 40, 0.8714878434, 425.0, 12000.0, 0, 6.0,
                      16.0, 1.0, 1.0, 0.00164917222),
        PEG_STAGE_CAL(4, 3, 4, 3, 8, 40, 0.8714878434, 190.0, 3000.0, 0, 6.0,
                      16.0, 1.0, 1.0, 0.00164917222),
};

#undef PEG_STAGE_CAL

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

const PegTimeManagerStageCalibration *
peg_time_manager_default_stage_calibration(int bag_tiles, int stage_index,
                                           int fidelity_plies) {
  for (size_t index = 0;
       index < sizeof(PEG_TIME_MANAGER_STAGE_CALIBRATIONS) /
                   sizeof(PEG_TIME_MANAGER_STAGE_CALIBRATIONS[0]);
       index++) {
    const PegTimeManagerStageCalibration *calibration =
        &PEG_TIME_MANAGER_STAGE_CALIBRATIONS[index];
    if (calibration->bag_tiles == bag_tiles &&
        calibration->stage_index == stage_index &&
        calibration->fidelity_plies == fidelity_plies) {
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

double peg_time_manager_default_regret_reduction(
    const PegTimeManagerRequest *request,
    const PegTimeManagerCalibration *calibration, void *user_data) {
  (void)calibration;
  (void)user_data;
  if (request == NULL) {
    return NAN;
  }
  if (request->boundary_kind == PEG_TIME_MANAGER_BOUNDARY_FIRST_TWO_2PLY) {
    // The quality panel begins at the first-two checkpoint and therefore does
    // not identify greedy->2 directly. Give the minimum useful wave the same
    // order of value as the measured 2->4 block; completion risk, not this
    // weak prior, is intended to be its dominant admission gate.
    const PegTimeManagerValueObservation *two_to_four =
        peg_time_manager_default_2ply_value_observation(2, 4);
    return two_to_four != NULL ? two_to_four->observed_utility_gain : NAN;
  }
  if (request->boundary_kind != PEG_TIME_MANAGER_BOUNDARY_NEXT_2PLY_CANDIDATE) {
    return NAN;
  }
  const int completed = request->completed_2ply_candidates;
  if (completed >= 2 && completed < 4) {
    const PegTimeManagerValueObservation *two_to_four =
        peg_time_manager_default_2ply_value_observation(2, 4);
    return two_to_four != NULL ? two_to_four->observed_utility_gain / 2.0 : NAN;
  }
  if (completed >= 4 && completed < 8) {
    const PegTimeManagerValueObservation *four_to_eight =
        peg_time_manager_default_2ply_value_observation(4, 8);
    return four_to_eight != NULL ? four_to_eight->observed_utility_gain / 4.0
                                 : NAN;
  }
  return completed >= 8 ? PEG_TIME_MANAGER_POST8_VALUE_PER_CANDIDATE : NAN;
}

static bool peg_time_manager_positive_finite(double value) {
  return isfinite(value) && value > 0.0;
}

static bool peg_time_manager_nonnegative_finite(double value) {
  return isfinite(value) && value >= 0.0;
}

static bool peg_time_manager_scale_work(TimeManagerWork *work,
                                        double multiplier) {
  if (work == NULL || !isfinite(multiplier) || multiplier < 1.0) {
    return false;
  }
  const long double nodes = (long double)work->nodes * multiplier;
  const long double scenarios = (long double)work->scenarios * multiplier;
  const long double fixed_seconds =
      (long double)work->fixed_seconds * multiplier;
  if (nodes > UINT64_MAX || scenarios > UINT64_MAX ||
      !isfinite((double)fixed_seconds)) {
    return false;
  }
  work->nodes = (uint64_t)ceill(nodes);
  work->scenarios = (uint64_t)ceill(scenarios);
  work->fixed_seconds = (double)fixed_seconds;
  return isfinite(work->fixed_seconds);
}

static TimeManagerWork
peg_time_manager_half_expected_work(const TimeManagerWork *pair_work) {
  TimeManagerWork work = *pair_work;
  work.nodes = (work.nodes + 1) / 2;
  work.scenarios = (work.scenarios + 1) / 2;
  work.candidates = 1;
  work.fixed_seconds *= 0.5;
  return work;
}

double peg_time_manager_estimate_minimum_2ply_seconds(
    const PegTimeManagerCalibration *calibration,
    const TimeManagerCostModel *expected_cost_model, int minimum_candidates) {
  if (calibration == NULL || expected_cost_model == NULL ||
      calibration->boundary_kind != PEG_TIME_MANAGER_BOUNDARY_FIRST_TWO_2PLY ||
      calibration->candidates != 2 || minimum_candidates < 2) {
    return NAN;
  }
  const double first_wave_seconds = time_manager_estimate_seconds(
      expected_cost_model, &calibration->median_work);
  const TimeManagerWork single_candidate_work =
      peg_time_manager_half_expected_work(&calibration->median_work);
  const double single_candidate_seconds = time_manager_estimate_seconds(
      expected_cost_model, &single_candidate_work);
  if (!peg_time_manager_nonnegative_finite(first_wave_seconds) ||
      !peg_time_manager_nonnegative_finite(single_candidate_seconds)) {
    return NAN;
  }
  const double total_seconds =
      first_wave_seconds +
      (double)(minimum_candidates - 2) * single_candidate_seconds;
  return peg_time_manager_nonnegative_finite(total_seconds) ? total_seconds
                                                            : NAN;
}

// A completed prefix can only calibrate the work dimensions it actually
// exercised. In particular, a cheap greedy/2-ply prefix with zero endgame
// nodes says nothing about the rate of a later candidate whose tail is
// dominated by an exact endgame. Scaling that unobserved coefficient down was
// enough to admit such a candidate in pair 45 of the 2026-07-31 match; it then
// consumed the rest of the PEG window without completing. Keep the cold-start
// estimate for unobserved dimensions so the completion gate remains
// conservative.
static void
peg_time_manager_scale_observed_costs(TimeManagerCostModel *model, double scale,
                                      uint64_t completed_scenarios,
                                      uint64_t completed_endgame_nodes) {
  model->peg_fixed_seconds_per_chunk *= scale;
  if (completed_scenarios > 0) {
    model->peg_seconds_per_scenario *= scale;
  }
  // A completed prefix cannot justify scaling an unseen dimension downward,
  // but a slow prefix is evidence that every cold coefficient should become
  // more conservative. This asymmetry fixes the pair-45 failure without
  // ignoring thermal/load slowdowns before the first exact-endgame leaf.
  if (completed_endgame_nodes > 0 || scale > 1.0) {
    model->peg_seconds_per_endgame_node *= scale;
  }
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

bool peg_time_manager_runtime_rate_update(PegTimeManagerRuntimeRate *rate,
                                          int workers, double stage_seconds,
                                          uint64_t stage_endgame_nodes) {
  if (rate == NULL || workers < 1 || !isfinite(stage_seconds) ||
      stage_seconds < 0.01 || stage_endgame_nodes == 0) {
    return false;
  }
  const double observed_normalized_rate =
      stage_seconds / (double)stage_endgame_nodes * (double)workers;
  if (!peg_time_manager_positive_finite(observed_normalized_rate)) {
    return false;
  }
  if (rate->observations == 0) {
    rate->expected_normalized_seconds_per_endgame_node =
        observed_normalized_rate;
    rate->deadline_normalized_seconds_per_endgame_node =
        observed_normalized_rate;
  } else {
    if (!peg_time_manager_positive_finite(
            rate->expected_normalized_seconds_per_endgame_node) ||
        !peg_time_manager_positive_finite(
            rate->deadline_normalized_seconds_per_endgame_node)) {
      return false;
    }
    rate->expected_normalized_seconds_per_endgame_node =
        0.8 * rate->expected_normalized_seconds_per_endgame_node +
        0.2 * observed_normalized_rate;
    rate->deadline_normalized_seconds_per_endgame_node =
        fmax(0.98 * rate->deadline_normalized_seconds_per_endgame_node,
             observed_normalized_rate);
  }
  rate->observations++;
  return true;
}

bool peg_time_manager_runtime_rate_apply(const PegTimeManagerRuntimeRate *rate,
                                         int workers,
                                         double deadline_safety_multiplier,
                                         TimeManagerCostModel *expected_cost,
                                         TimeManagerCostModel *deadline_cost) {
  if (rate == NULL || rate->observations == 0 || workers < 1 ||
      !isfinite(deadline_safety_multiplier) ||
      deadline_safety_multiplier < 1.0 || expected_cost == NULL ||
      deadline_cost == NULL ||
      !peg_time_manager_positive_finite(
          rate->expected_normalized_seconds_per_endgame_node) ||
      !peg_time_manager_positive_finite(
          rate->deadline_normalized_seconds_per_endgame_node)) {
    return false;
  }
  const double expected_rate =
      rate->expected_normalized_seconds_per_endgame_node / (double)workers;
  const double deadline_rate =
      rate->deadline_normalized_seconds_per_endgame_node / (double)workers *
      deadline_safety_multiplier;
  if (!peg_time_manager_positive_finite(expected_rate) ||
      !peg_time_manager_positive_finite(deadline_rate)) {
    return false;
  }
  expected_cost->peg_seconds_per_endgame_node =
      fmax(expected_cost->peg_seconds_per_endgame_node, expected_rate);
  deadline_cost->peg_seconds_per_endgame_node =
      fmax(deadline_cost->peg_seconds_per_endgame_node, deadline_rate);
  return true;
}

static bool
peg_time_manager_request_matches(const PegTimeManagerCalibration *calibration,
                                 const PegTimeManagerRequest *request) {
  const bool is_post_wave =
      request->boundary_kind == PEG_TIME_MANAGER_BOUNDARY_NEXT_2PLY_CANDIDATE;
  const bool boundary_matches =
      request->boundary_kind == calibration->boundary_kind ||
      (is_post_wave &&
       calibration->boundary_kind == PEG_TIME_MANAGER_BOUNDARY_FIRST_TWO_2PLY);
  return boundary_matches && request->bag_tiles == calibration->bag_tiles &&
         request->stage_index == calibration->stage_index &&
         request->fidelity_plies == calibration->fidelity_plies &&
         request->workers > 0 &&
         request->candidates == (is_post_wave ? 1 : calibration->candidates) &&
         (is_post_wave ? request->completed_2ply_candidates >= 2
                       : request->completed_2ply_candidates ==
                             calibration->required_completed_2ply_candidates) &&
         request->nested_enabled == calibration->nested_enabled &&
         request->scenario_stride == calibration->scenario_stride &&
         request->parallel_wave_dispatch ==
             (is_post_wave ? false : calibration->parallel_wave_dispatch);
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
      peg_time_manager_default_calibration(
          request->boundary_kind ==
                  PEG_TIME_MANAGER_BOUNDARY_NEXT_2PLY_CANDIDATE
              ? PEG_TIME_MANAGER_BOUNDARY_FIRST_TWO_2PLY
              : request->boundary_kind,
          request->bag_tiles);
  if (calibration == NULL) {
    return decision;
  }
  decision.uses_post_wave_tail_proxy =
      request->boundary_kind == PEG_TIME_MANAGER_BOUNDARY_NEXT_2PLY_CANDIDATE;
  decision.pricing_work =
      decision.uses_post_wave_tail_proxy
          ? peg_time_manager_half_expected_work(&calibration->median_work)
          : calibration->median_work;
  // A separately calibrated one-candidate tail is still missing. Do not halve
  // the pair's p99/max: retaining the whole first-two envelope is the explicit
  // conservative proxy for each later candidate.
  decision.empirical_p99_work = calibration->empirical_p99_work;
  decision.provisional_completion_bound_work = calibration->empirical_max_work;
  if (decision.uses_post_wave_tail_proxy) {
    decision.empirical_p99_work.candidates = 1;
    decision.provisional_completion_bound_work.candidates = 1;
  }
  const double completion_bound_multiplier =
      policy->completion_bound_multiplier > 0.0
          ? policy->completion_bound_multiplier
          : 1.0;
  if (!peg_time_manager_scale_work(&decision.provisional_completion_bound_work,
                                   completion_bound_multiplier)) {
    return decision;
  }
  decision.configuration_matches =
      peg_time_manager_request_matches(calibration, request);
  if (!decision.configuration_matches) {
    return decision;
  }

  TimeManagerCostModel expected_cost_model = policy->expected_cost_model;
  TimeManagerCostModel deadline_cost_model = policy->deadline_cost_model;
  decision.live_cost_scale = 1.0;
  if (policy->use_live_cost_scale && request->elapsed_seconds >= 0.01 &&
      (request->completed_scenarios > 0 ||
       request->completed_endgame_nodes > 0)) {
    const TimeManagerWork completed_work = {
        .mode = ANALYSIS_MODE_PEG,
        .nodes = request->completed_endgame_nodes,
        .scenarios = request->completed_scenarios,
    };
    const double modeled_elapsed =
        time_manager_estimate_seconds(&expected_cost_model, &completed_work);
    if (isfinite(modeled_elapsed) && modeled_elapsed > 0.0) {
      // The completed prefix includes pruning/movegen work not represented by
      // the two portable coordinates. Treating it as PEG work makes the scale
      // conservative. Clamp a single short prefix so timer noise or a very
      // unusual position cannot make the next deadline estimate absurd.
      decision.live_cost_scale = request->elapsed_seconds / modeled_elapsed;
      if (decision.live_cost_scale < 0.25) {
        decision.live_cost_scale = 0.25;
      } else if (decision.live_cost_scale > 4.0) {
        decision.live_cost_scale = 4.0;
      }
      peg_time_manager_scale_observed_costs(
          &expected_cost_model, decision.live_cost_scale,
          request->completed_scenarios, request->completed_endgame_nodes);
      peg_time_manager_scale_observed_costs(
          &deadline_cost_model, decision.live_cost_scale,
          request->completed_scenarios, request->completed_endgame_nodes);
    }
  }

  decision.pricing_seconds = time_manager_estimate_seconds(
      &expected_cost_model, &decision.pricing_work);
  decision.empirical_p99_seconds = time_manager_estimate_seconds(
      &deadline_cost_model, &decision.empirical_p99_work);
  decision.provisional_completion_bound_seconds = time_manager_estimate_seconds(
      &deadline_cost_model, &decision.provisional_completion_bound_work);
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
  // Requests report the clock/window still available *now*. Reconstruct the
  // turn-start bank before adding elapsed work as the committed current cost;
  // otherwise elapsed time would be charged twice.
  clock.remaining_seconds =
      (request->has_player_clock ? request->player_clock_remaining_seconds
                                 : request->remaining_seconds) +
      request->elapsed_seconds;
  clock.committed_current_seconds = request->elapsed_seconds;
  if (policy->future_reserve_endgame_nodes > 0) {
    if (!isfinite(policy->future_rate_safety_multiplier) ||
        policy->future_rate_safety_multiplier < 1.0 ||
        !peg_time_manager_nonnegative_finite(
            expected_cost_model.peg_seconds_per_endgame_node)) {
      return (PegTimeManagerDecision){0};
    }
    const double future_node_seconds =
        (double)policy->future_reserve_endgame_nodes *
        expected_cost_model.peg_seconds_per_endgame_node *
        policy->future_rate_safety_multiplier;
    if (!peg_time_manager_nonnegative_finite(future_node_seconds)) {
      return (PegTimeManagerDecision){0};
    }
    clock.future_reserve_seconds += future_node_seconds;
  }
  const TimeManagerChunk chunk = {
      .boundary = decision.uses_post_wave_tail_proxy
                      ? TIME_MANAGER_BOUNDARY_PEG_CANDIDATE
                      : TIME_MANAGER_BOUNDARY_PEG_WAVE,
      .work = decision.pricing_work,
      .has_completion_bound = decision.has_provisional_completion_bound,
      .completion_bound_work = decision.provisional_completion_bound_work,
      .completion_confidence = decision.completion_confidence,
      .expected_regret_reduction = expected_regret_reduction,
  };
  decision.plan = time_manager_plan(&clock, &expected_cost_model,
                                    &deadline_cost_model, &chunk, 1);
  if (!decision.plan.valid) {
    return (PegTimeManagerDecision){0};
  }

  const bool physically_fits = decision.has_provisional_completion_bound &&
                               decision.provisional_completion_bound_seconds <=
                                   request->remaining_seconds;
  decision.valid = true;
  // The default remains shadow-only. This branch's live PlayChooser policy
  // explicitly opts into the finite-corpus empirical maximum, a safety
  // multiplier, and the lower 95% evidence threshold. That is operationally
  // enforceable but must not be described as a certified p99 guarantee.
  decision.safe_to_enforce =
      policy->allow_provisional_enforcement &&
      completion_bound_multiplier >= 1.0 &&
      decision.completion_confidence >= clock.minimum_completion_confidence;
  decision.should_start = physically_fits && decision.plan.chunks_bought == 1;
  return decision;
}

static bool peg_time_manager_predict_stage_nodes(
    const PegTimeManagerStageCalibration *calibration,
    const PegTimeManagerStageRequest *request, bool completion_bound,
    uint64_t *nodes_out) {
  if (calibration == NULL || request == NULL || nodes_out == NULL) {
    return false;
  }
  const double nodes_per_scenario =
      completion_bound ? calibration->completion_nodes_per_scenario
                       : calibration->expected_nodes_per_scenario;
  const double previous_node_multiplier =
      completion_bound ? calibration->completion_previous_node_multiplier
                       : calibration->expected_previous_node_multiplier;
  if (!peg_time_manager_nonnegative_finite(nodes_per_scenario) ||
      !peg_time_manager_nonnegative_finite(previous_node_multiplier)) {
    return false;
  }
  const long double cold_nodes =
      (long double)request->stage_scenarios * nodes_per_scenario;
  const long double bounded_cold_nodes =
      completion_bound && calibration->completion_absolute_nodes > 0
          ? fminl(cold_nodes,
                  (long double)calibration->completion_absolute_nodes)
          : cold_nodes;
  const long double prior_nodes =
      (long double)request->previous_stage_endgame_nodes *
      previous_node_multiplier;
  const long double predicted_nodes = fmaxl(bounded_cold_nodes, prior_nodes);
  if (predicted_nodes < 0.0L || predicted_nodes > UINT64_MAX) {
    return false;
  }
  *nodes_out = (uint64_t)ceill(predicted_nodes);
  return true;
}

static bool peg_time_manager_stage_request_matches(
    const PegTimeManagerPolicy *policy,
    const PegTimeManagerStageCalibration *calibration,
    const PegTimeManagerStageRequest *request) {
  if (policy == NULL || calibration == NULL || request == NULL) {
    return false;
  }
  const bool previous_work_matches =
      calibration->previous_fidelity_plies == 0
          ? request->previous_stage_endgame_nodes == 0
          : request->previous_stage_scenarios == request->stage_scenarios;
  return policy->use_complete_stage_admission &&
         request->bag_tiles == calibration->bag_tiles &&
         request->stage_index == calibration->stage_index &&
         request->fidelity_plies == calibration->fidelity_plies &&
         request->previous_fidelity_plies ==
             calibration->previous_fidelity_plies &&
         request->workers > 0 && request->candidates >= 2 &&
         request->candidates <= calibration->maximum_candidates &&
         !request->nested_enabled && request->scenario_stride == 1 &&
         request->parallel_stage_dispatch && request->stage_scenarios > 0 &&
         previous_work_matches;
}

PegTimeManagerDecision
peg_time_manager_plan_stage(const PegTimeManagerPolicy *policy,
                            const PegTimeManagerStageRequest *request) {
  PegTimeManagerDecision decision = {.is_complete_stage = true};
  if (policy == NULL || request == NULL || request->bag_tiles < 1 ||
      request->bag_tiles > 4 || request->stage_index < 1 ||
      request->fidelity_plies < 2 || !isfinite(request->elapsed_seconds) ||
      request->elapsed_seconds < 0.0 ||
      !isfinite(request->observed_previous_stage_elapsed_seconds) ||
      request->observed_previous_stage_elapsed_seconds < 0.0 ||
      isnan(request->remaining_seconds) || request->remaining_seconds < 0.0 ||
      (request->has_player_clock &&
       (!isfinite(request->player_clock_remaining_seconds) ||
        request->player_clock_remaining_seconds < 0.0))) {
    return decision;
  }

  const PegTimeManagerStageCalibration *calibration =
      peg_time_manager_default_stage_calibration(
          request->bag_tiles, request->stage_index, request->fidelity_plies);
  if (calibration == NULL) {
    return decision;
  }
  decision.configuration_matches =
      peg_time_manager_stage_request_matches(policy, calibration, request);
  if (!decision.configuration_matches) {
    return decision;
  }

  decision.pricing_work = (TimeManagerWork){
      .mode = ANALYSIS_MODE_PEG,
      .scenarios = request->stage_scenarios,
      .candidates = (uint64_t)request->candidates,
  };
  decision.empirical_p99_work = decision.pricing_work;
  if (!peg_time_manager_predict_stage_nodes(calibration, request,
                                            /*completion_bound=*/false,
                                            &decision.pricing_work.nodes) ||
      !peg_time_manager_predict_stage_nodes(
          calibration, request, /*completion_bound=*/true,
          &decision.empirical_p99_work.nodes)) {
    return (PegTimeManagerDecision){.is_complete_stage = true};
  }
  decision.provisional_completion_bound_work = decision.empirical_p99_work;
  const double completion_bound_multiplier =
      policy->completion_bound_multiplier > 0.0
          ? policy->completion_bound_multiplier
          : 1.0;
  if (!peg_time_manager_scale_work(&decision.provisional_completion_bound_work,
                                   completion_bound_multiplier)) {
    return (PegTimeManagerDecision){.is_complete_stage = true};
  }

  TimeManagerCostModel expected_cost_model = policy->expected_cost_model;
  TimeManagerCostModel deadline_cost_model = policy->deadline_cost_model;
  const double deadline_residual =
      policy->has_runtime_endgame_rate
          ? calibration->warm_deadline_cost_multiplier
          : calibration->deadline_cost_multiplier;
  if (!isfinite(deadline_residual) || deadline_residual < 1.0) {
    return (PegTimeManagerDecision){.is_complete_stage = true};
  }
  deadline_cost_model.peg_fixed_seconds_per_chunk *= deadline_residual;
  deadline_cost_model.peg_seconds_per_scenario *= deadline_residual;
  deadline_cost_model.peg_seconds_per_endgame_node *= deadline_residual;
  deadline_cost_model.peg_seconds_per_candidate *= deadline_residual;
  decision.live_cost_scale = 1.0;
  const bool has_previous_stage_rate =
      request->observed_previous_stage_elapsed_seconds >= 0.01 &&
      (request->observed_previous_stage_scenarios > 0 ||
       request->observed_previous_stage_endgame_nodes > 0);
  const double observed_elapsed =
      has_previous_stage_rate ? request->observed_previous_stage_elapsed_seconds
                              : request->elapsed_seconds;
  const uint64_t observed_scenarios =
      has_previous_stage_rate ? request->observed_previous_stage_scenarios
                              : request->completed_scenarios;
  const uint64_t observed_endgame_nodes =
      has_previous_stage_rate ? request->observed_previous_stage_endgame_nodes
                              : request->completed_endgame_nodes;
  if (policy->use_live_cost_scale && observed_elapsed >= 0.01 &&
      (observed_scenarios > 0 || observed_endgame_nodes > 0)) {
    const TimeManagerWork completed_work = {
        .mode = ANALYSIS_MODE_PEG,
        .nodes = observed_endgame_nodes,
        .scenarios = observed_scenarios,
    };
    const double modeled_elapsed =
        time_manager_estimate_seconds(&expected_cost_model, &completed_work);
    if (isfinite(modeled_elapsed) && modeled_elapsed > 0.0) {
      decision.live_cost_scale = observed_elapsed / modeled_elapsed;
      if (decision.live_cost_scale < 0.25) {
        decision.live_cost_scale = 0.25;
      } else if (decision.live_cost_scale > 4.0) {
        decision.live_cost_scale = 4.0;
      }
      peg_time_manager_scale_observed_costs(
          &expected_cost_model, decision.live_cost_scale, observed_scenarios,
          observed_endgame_nodes);
      // Greedy work can show that move generation is fast, but it cannot show
      // that this position's exact-endgame leaves are fast. Until a completed
      // endgame-bearing stage exists, never shrink any part of the deadline
      // envelope from that node-free observation. A slow greedy prefix still
      // expands every coefficient.
      const double deadline_live_scale =
          observed_endgame_nodes > 0 ? decision.live_cost_scale
                                     : fmax(1.0, decision.live_cost_scale);
      peg_time_manager_scale_observed_costs(
          &deadline_cost_model, deadline_live_scale, observed_scenarios,
          observed_endgame_nodes);
    }
  }

  decision.pricing_seconds = time_manager_estimate_seconds(
      &expected_cost_model, &decision.pricing_work);
  decision.empirical_p99_seconds = time_manager_estimate_seconds(
      &deadline_cost_model, &decision.empirical_p99_work);
  decision.provisional_completion_bound_seconds = time_manager_estimate_seconds(
      &deadline_cost_model, &decision.provisional_completion_bound_work);
  if (!peg_time_manager_nonnegative_finite(decision.pricing_seconds) ||
      !peg_time_manager_nonnegative_finite(decision.empirical_p99_seconds) ||
      decision.empirical_p99_seconds < decision.pricing_seconds ||
      !peg_time_manager_nonnegative_finite(
          decision.provisional_completion_bound_seconds) ||
      decision.provisional_completion_bound_seconds <
          decision.empirical_p99_seconds) {
    return (PegTimeManagerDecision){.is_complete_stage = true};
  }

  decision.expected_regret_reduction = calibration->expected_regret_reduction;
  if (!peg_time_manager_nonnegative_finite(
          decision.expected_regret_reduction)) {
    return (PegTimeManagerDecision){.is_complete_stage = true};
  }
  decision.has_provisional_completion_bound = true;
  decision.completion_confidence =
      calibration->provisional_completion_confidence;
  decision.heldout_gate_passed = calibration->heldout_gate_passed;

  TimeManagerClock clock = policy->clock;
  clock.remaining_seconds =
      (request->has_player_clock ? request->player_clock_remaining_seconds
                                 : request->remaining_seconds) +
      request->elapsed_seconds;
  clock.committed_current_seconds = request->elapsed_seconds;
  if (policy->future_reserve_endgame_nodes > 0) {
    if (!isfinite(policy->future_rate_safety_multiplier) ||
        policy->future_rate_safety_multiplier < 1.0 ||
        !peg_time_manager_nonnegative_finite(
            expected_cost_model.peg_seconds_per_endgame_node)) {
      return (PegTimeManagerDecision){.is_complete_stage = true};
    }
    const double future_node_seconds =
        (double)policy->future_reserve_endgame_nodes *
        expected_cost_model.peg_seconds_per_endgame_node *
        policy->future_rate_safety_multiplier;
    if (!peg_time_manager_nonnegative_finite(future_node_seconds)) {
      return (PegTimeManagerDecision){.is_complete_stage = true};
    }
    clock.future_reserve_seconds += future_node_seconds;
  }
  const TimeManagerChunk chunk = {
      .boundary = TIME_MANAGER_BOUNDARY_PEG_STAGE,
      .work = decision.pricing_work,
      .has_completion_bound = true,
      .completion_bound_work = decision.provisional_completion_bound_work,
      .completion_confidence = decision.completion_confidence,
      .expected_regret_reduction = decision.expected_regret_reduction,
  };
  decision.plan = time_manager_plan(&clock, &expected_cost_model,
                                    &deadline_cost_model, &chunk, 1);
  if (!decision.plan.valid) {
    return (PegTimeManagerDecision){.is_complete_stage = true};
  }

  const bool physically_fits = decision.provisional_completion_bound_seconds <=
                               request->remaining_seconds;
  decision.valid = true;
  decision.safe_to_enforce =
      policy->allow_provisional_enforcement &&
      calibration->heldout_gate_passed && completion_bound_multiplier >= 1.0 &&
      decision.completion_confidence >= clock.minimum_completion_confidence &&
      (request->stage_index > 1 || policy->has_runtime_endgame_rate);
  decision.should_start = physically_fits && decision.plan.chunks_bought == 1;
  return decision;
}
