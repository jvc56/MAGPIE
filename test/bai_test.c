#include "../src/compat/cpthread.h"
#include "../src/compat/ctime.h"
#include "../src/def/bai_defs.h"
#include "../src/def/cpthread_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/bai_result.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/xoshiro.h"
#include "../src/impl/bai.h"
#include "../src/impl/bai_logger.h"
#include "../src/impl/random_variable.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum { NUM_UNIQUE_MEANS = 10000 };

static const int sampling_rules[3] = {
    BAI_SAMPLING_RULE_ROUND_ROBIN,
    BAI_SAMPLING_RULE_TOP_TWO_IDS,
};

static const int num_sampling_rules = sizeof(sampling_rules) / sizeof(int);

static const int strategies[][3] = {
    {BAI_SAMPLING_RULE_TOP_TWO_IDS, BAI_THRESHOLD_GK16},
};
static const int num_strategies_entries =
    sizeof(strategies) / sizeof(strategies[0]);

void bai_wrapper_with_sim_results(BAIOptions *bai_options, RandomVariables *rvs,
                                  RandomVariables *rng,
                                  ThreadControl *thread_control,
                                  BAILogger *bai_logger,
                                  const SimResults *rule_zero_sim_results,
                                  BAIResult *bai_result) {
  bai_options->parent_worker_thread_index = 0;
  thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED);
  bai(bai_options, rvs, rng, thread_control, bai_logger,
      /*progress_listener=*/NULL, rule_zero_sim_results, bai_result);
}

void bai_wrapper(BAIOptions *bai_options, RandomVariables *rvs,
                 RandomVariables *rng, ThreadControl *thread_control,
                 BAILogger *bai_logger, BAIResult *bai_result) {
  bai_wrapper_with_sim_results(bai_options, rvs, rng, thread_control,
                               bai_logger, /*rule_zero_sim_results=*/NULL,
                               bai_result);
}

void test_bai_top_two(int num_threads) {
  // The winning arm's spread is kept small so its empirical mean cannot drift
  // to >= 1.0, which (with cutoff == 0) would trip an early WIN_PCT_CUTOFF stop
  // before the GK16 threshold is reached. A variance of 1 here leaves the
  // initial-phase mean of a 0.9 arm above 1.0 for a meaningful fraction of
  // sample sequences.
  const double means_and_vars[] = {0.1, 1, 0.9, 0.05};
  const int num_rvs = (sizeof(means_and_vars)) / (sizeof(double) * 2);
  RandomVariablesArgs rv_args = {
      .type = RANDOM_VARIABLES_NORMAL,
      .num_rvs = num_rvs,
      .means_and_vars = means_and_vars,
      .seed = 10,
  };
  RandomVariables *rvs = rvs_create(&rv_args);

  RandomVariablesArgs rng_args = {
      .type = RANDOM_VARIABLES_UNIFORM,
      .num_rvs = num_rvs,
      .seed = 10,
  };
  RandomVariables *rng = rvs_create(&rng_args);

  BAIOptions bai_options = {
      .sampling_rule = BAI_SAMPLING_RULE_TOP_TWO_IDS,
      .threshold = BAI_THRESHOLD_GK16,
      .delta = 0.05,
      .sample_minimum = 50,
      .sample_limit = 200,
      .time_limit_seconds = 0,
      .num_threads = num_threads,
      .cutoff = 0,
  };

  ThreadControl *thread_control = thread_control_create();
  BAIResult *bai_result = bai_result_create();
  bai_wrapper(&bai_options, rvs, rng, thread_control, NULL, bai_result);
  assert(bai_result_get_status(bai_result) == BAI_RESULT_STATUS_THRESHOLD);
  assert(bai_result_get_best_arm(bai_result) == 1);
  thread_control_destroy(thread_control);
  bai_result_destroy(bai_result);
  rvs_destroy(rng);
  rvs_destroy(rvs);
}

void test_bai_sample_limit(int num_threads) {
  const double means_and_vars[] = {0.1, 1, 0.5, 1, 0.2, 1};
  const uint64_t num_rvs = (sizeof(means_and_vars)) / (sizeof(double) * 2);
  RandomVariablesArgs rv_args = {
      .type = RANDOM_VARIABLES_NORMAL,
      .num_rvs = num_rvs,
      .means_and_vars = means_and_vars,
      .seed = 10,
  };
  RandomVariables *rvs = rvs_create(&rv_args);

  RandomVariablesArgs rng_args = {
      .type = RANDOM_VARIABLES_UNIFORM,
      .num_rvs = num_rvs,
      .seed = 10,
  };
  RandomVariables *rng = rvs_create(&rng_args);

  BAIOptions bai_options = {
      .threshold = BAI_THRESHOLD_NONE,
      .delta = 0.05,
      .sample_minimum = 37,
      .sample_limit = 200,
      .time_limit_seconds = 0,
      .num_threads = num_threads,
      .cutoff = 0,
  };
  ThreadControl *thread_control = thread_control_create();
  BAIResult *bai_result = bai_result_create();
  for (int i = 0; i < num_sampling_rules; i++) {
    bai_options.sampling_rule = sampling_rules[i];
    rvs_reset(rvs, &rv_args);
    bai_wrapper(&bai_options, rvs, rng, thread_control, NULL, bai_result);
    assert(bai_result_get_status(bai_result) == BAI_RESULT_STATUS_SAMPLE_LIMIT);
    assert(bai_result_get_best_arm(bai_result) == 1);
    uint64_t expected_num_samples = bai_options.sample_limit;
    if (expected_num_samples < num_rvs * bai_options.sample_minimum) {
      expected_num_samples = num_rvs * bai_options.sample_minimum;
    }
    assert(rvs_get_total_samples(rvs) == expected_num_samples);
  }
  thread_control_destroy(thread_control);
  // The timer should stop once the BAI has finished.
  const double bai_time_elapsed = bai_result_get_elapsed_seconds(bai_result);
  ctime_nap(0.2);
  assert(bai_time_elapsed == bai_result_get_elapsed_seconds(bai_result));
  bai_result_destroy(bai_result);
  rvs_destroy(rng);
  rvs_destroy(rvs);
}

void test_bai_regret_limit(int num_threads) {
  // The winner is deliberately well separated so this test exercises the
  // value-of-computation exit without depending on a marginal noisy ranking.
  const double means_and_vars[] = {
      0.2, 0.0025, 0.7, 0.0025, 0.1, 0.0025,
  };
  const uint64_t num_rvs = (sizeof(means_and_vars)) / (sizeof(double) * 2);
  RandomVariablesArgs rv_args = {
      .type = RANDOM_VARIABLES_NORMAL,
      .num_rvs = num_rvs,
      .means_and_vars = means_and_vars,
      .seed = 10,
  };
  RandomVariables *rvs = rvs_create(&rv_args);

  RandomVariablesArgs rng_args = {
      .type = RANDOM_VARIABLES_UNIFORM,
      .num_rvs = num_rvs,
      .seed = 10,
  };
  RandomVariables *rng = rvs_create(&rng_args);

  BAIOptions bai_options = {
      .sampling_rule = BAI_SAMPLING_RULE_TOP_TWO_IDS,
      .threshold = BAI_THRESHOLD_NONE,
      .delta = 0.05,
      .sample_minimum = 32,
      .sample_limit = 100000,
      .time_limit_seconds = 0,
      .num_threads = num_threads,
      .cutoff = 0,
      .regret_stop_target = 0.0001,
      .regret_cross_arm_correlation = 0.48,
      .regret_calibration = 1.0,
      .regret_check_interval = 32,
      .regret_min_samples_per_arm = 32,
  };
  ThreadControl *thread_control = thread_control_create();
  BAIResult *bai_result = bai_result_create();
  bai_wrapper(&bai_options, rvs, rng, thread_control, NULL, bai_result);
  assert(bai_result_get_status(bai_result) == BAI_RESULT_STATUS_REGRET_LIMIT);
  assert(bai_result_get_best_arm(bai_result) == 1);
  assert(rvs_get_total_samples(rvs) < bai_options.sample_limit);
  const double estimated_regret = bai_result_get_estimated_regret(bai_result);
  const double joint_estimated_regret =
      bai_result_get_joint_estimated_regret(bai_result);
  const double regret_at_stop = bai_result_get_regret_at_stop(bai_result);
  const double joint_regret_at_stop =
      bai_result_get_joint_regret_at_stop(bai_result);
  assert(isfinite(estimated_regret));
  assert(isfinite(joint_estimated_regret));
  assert(isfinite(regret_at_stop));
  assert(isfinite(joint_regret_at_stop));
  assert(joint_estimated_regret >= estimated_regret);
  assert(joint_regret_at_stop >= regret_at_stop);
  assert(bai_result_get_near_tie_challengers(bai_result) >= 0);
  assert(bai_result_get_near_tie_challengers_at_stop(bai_result) >= 0);
  assert(regret_at_stop <= bai_options.regret_stop_target);

  thread_control_destroy(thread_control);
  bai_result_destroy(bai_result);
  rvs_destroy(rng);
  rvs_destroy(rvs);
}

static void test_bai_rule_zero_stop(int num_threads) {
  // Keep the incumbent well separated: Rule Zero may only stop after the
  // existing 99%-CI near-tie diagnostic reports no challenger.
  const double means_and_vars[] = {
      0.2, 0.0025, 0.7, 0.0025, 0.1, 0.0025,
  };
  const uint64_t num_rvs = (sizeof(means_and_vars)) / (sizeof(double) * 2);
  RandomVariablesArgs rv_args = {
      .type = RANDOM_VARIABLES_NORMAL,
      .num_rvs = num_rvs,
      .means_and_vars = means_and_vars,
      .seed = 10,
  };
  RandomVariables *rvs = rvs_create(&rv_args);
  RandomVariablesArgs rng_args = {
      .type = RANDOM_VARIABLES_UNIFORM,
      .num_rvs = num_rvs,
      .seed = 10,
  };
  RandomVariables *rng = rvs_create(&rng_args);
  SimResults *sim_results = sim_results_create(0);
  sim_results_increment_node_count(sim_results);

  BAIOptions bai_options = {
      .sampling_rule = BAI_SAMPLING_RULE_TOP_TWO_IDS,
      .threshold = BAI_THRESHOLD_NONE,
      .delta = 0.05,
      .sample_minimum = 32,
      .sample_limit = 100000,
      .time_limit_seconds = 0,
      .num_threads = num_threads,
      .cutoff = 0,
      .rule_zero_enabled = true,
      .rule_zero_minimum_nodes = 1,
      .rule_zero_minimum_stable_checkpoints = 2,
      .rule_zero_checkpoint_interval = 32,
  };
  ThreadControl *thread_control = thread_control_create();
  BAIResult *bai_result = bai_result_create();
  bai_wrapper_with_sim_results(&bai_options, rvs, rng, thread_control, NULL,
                               sim_results, bai_result);
  assert(bai_result_get_status(bai_result) ==
         BAI_RESULT_STATUS_RULE_ZERO_LIMIT);
  assert(bai_result_get_rule_zero_stopped(bai_result));
  assert(bai_result_get_rule_zero_would_stop(bai_result));
  assert(bai_result_get_rule_zero_stop_nodes(bai_result) >= 1);
  assert(bai_result_get_rule_zero_stop_iterations(bai_result) <
         bai_options.sample_limit);
  assert(bai_result_get_rule_zero_stable_checkpoints(bai_result) >= 2);
  assert(bai_result_get_near_tie_challengers_at_stop(bai_result) == 0);

  bai_result_destroy(bai_result);
  thread_control_destroy(thread_control);
  sim_results_destroy(sim_results);
  rvs_destroy(rng);
  rvs_destroy(rvs);
}

static void
test_bai_rule_zero_fails_closed_without_work_counter(int num_threads) {
  const double means_and_vars[] = {
      0.2, 0.0025, 0.7, 0.0025, 0.1, 0.0025,
  };
  const uint64_t num_rvs = (sizeof(means_and_vars)) / (sizeof(double) * 2);
  RandomVariablesArgs rv_args = {
      .type = RANDOM_VARIABLES_NORMAL,
      .num_rvs = num_rvs,
      .means_and_vars = means_and_vars,
      .seed = 10,
  };
  RandomVariables *rvs = rvs_create(&rv_args);
  RandomVariablesArgs rng_args = {
      .type = RANDOM_VARIABLES_UNIFORM,
      .num_rvs = num_rvs,
      .seed = 10,
  };
  RandomVariables *rng = rvs_create(&rng_args);
  BAIOptions bai_options = {
      .sampling_rule = BAI_SAMPLING_RULE_TOP_TWO_IDS,
      .threshold = BAI_THRESHOLD_NONE,
      .delta = 0.05,
      .sample_minimum = 32,
      .sample_limit = 320,
      .time_limit_seconds = 0,
      .num_threads = num_threads,
      .cutoff = 0,
      .rule_zero_enabled = true,
      .rule_zero_minimum_nodes = 1,
      .rule_zero_minimum_stable_checkpoints = 2,
      .rule_zero_checkpoint_interval = 32,
  };
  ThreadControl *thread_control = thread_control_create();
  BAIResult *bai_result = bai_result_create();
  bai_wrapper_with_sim_results(&bai_options, rvs, rng, thread_control, NULL,
                               /*rule_zero_sim_results=*/NULL, bai_result);
  assert(bai_result_get_status(bai_result) == BAI_RESULT_STATUS_SAMPLE_LIMIT);
  assert(!bai_result_get_rule_zero_stopped(bai_result));
  assert(!bai_result_get_rule_zero_would_stop(bai_result));
  assert(rvs_get_total_samples(rvs) == bai_options.sample_limit);

  bai_result_destroy(bai_result);
  thread_control_destroy(thread_control);
  rvs_destroy(rng);
  rvs_destroy(rvs);
}

static void test_bai_rule_zero_requires_zero_near_ties(int num_threads) {
  // Two statistically indistinguishable leading arms are a valid reason to
  // continue, even after the native-work and incumbent-stability thresholds
  // have been met.
  const double means_and_vars[] = {
      0.7, 0.0025, 0.7, 0.0025, 0.1, 0.0025,
  };
  const uint64_t num_rvs = (sizeof(means_and_vars)) / (sizeof(double) * 2);
  RandomVariablesArgs rv_args = {
      .type = RANDOM_VARIABLES_NORMAL,
      .num_rvs = num_rvs,
      .means_and_vars = means_and_vars,
      .seed = 10,
  };
  RandomVariables *rvs = rvs_create(&rv_args);
  RandomVariablesArgs rng_args = {
      .type = RANDOM_VARIABLES_UNIFORM,
      .num_rvs = num_rvs,
      .seed = 10,
  };
  RandomVariables *rng = rvs_create(&rng_args);
  SimResults *sim_results = sim_results_create(0);
  sim_results_increment_node_count(sim_results);
  BAIOptions bai_options = {
      .sampling_rule = BAI_SAMPLING_RULE_TOP_TWO_IDS,
      .threshold = BAI_THRESHOLD_NONE,
      .delta = 0.05,
      .sample_minimum = 32,
      .sample_limit = 320,
      .time_limit_seconds = 0,
      .num_threads = num_threads,
      .cutoff = 0,
      .rule_zero_enabled = true,
      .rule_zero_minimum_nodes = 1,
      .rule_zero_minimum_stable_checkpoints = 2,
      .rule_zero_checkpoint_interval = 32,
  };
  ThreadControl *thread_control = thread_control_create();
  BAIResult *bai_result = bai_result_create();
  bai_wrapper_with_sim_results(&bai_options, rvs, rng, thread_control, NULL,
                               sim_results, bai_result);
  assert(bai_result_get_status(bai_result) == BAI_RESULT_STATUS_SAMPLE_LIMIT);
  assert(!bai_result_get_rule_zero_stopped(bai_result));
  assert(!bai_result_get_rule_zero_would_stop(bai_result));
  assert(bai_result_get_near_tie_challengers(bai_result) > 0);

  bai_result_destroy(bai_result);
  thread_control_destroy(thread_control);
  sim_results_destroy(sim_results);
  rvs_destroy(rng);
  rvs_destroy(rvs);
}

static void
test_bai_rule_zero_shadow_records_without_stopping(int num_threads) {
  // Shadow mode must record the first satisfying checkpoint while the search
  // itself runs to its ordinary sample boundary, so a panel can compare the
  // would-stop choice against the full-horizon choice from one trace.
  const double means_and_vars[] = {
      0.2, 0.0025, 0.7, 0.0025, 0.1, 0.0025,
  };
  const uint64_t num_rvs = (sizeof(means_and_vars)) / (sizeof(double) * 2);
  RandomVariablesArgs rv_args = {
      .type = RANDOM_VARIABLES_NORMAL,
      .num_rvs = num_rvs,
      .means_and_vars = means_and_vars,
      .seed = 10,
  };
  RandomVariables *rvs = rvs_create(&rv_args);
  RandomVariablesArgs rng_args = {
      .type = RANDOM_VARIABLES_UNIFORM,
      .num_rvs = num_rvs,
      .seed = 10,
  };
  RandomVariables *rng = rvs_create(&rng_args);
  SimResults *sim_results = sim_results_create(0);
  sim_results_increment_node_count(sim_results);

  BAIOptions bai_options = {
      .sampling_rule = BAI_SAMPLING_RULE_TOP_TWO_IDS,
      .threshold = BAI_THRESHOLD_NONE,
      .delta = 0.05,
      .sample_minimum = 32,
      .sample_limit = 3200,
      .time_limit_seconds = 0,
      .num_threads = num_threads,
      .cutoff = 0,
      .rule_zero_enabled = true,
      .rule_zero_shadow = true,
      .rule_zero_minimum_nodes = 1,
      .rule_zero_minimum_stable_checkpoints = 2,
      .rule_zero_checkpoint_interval = 32,
  };
  ThreadControl *thread_control = thread_control_create();
  BAIResult *bai_result = bai_result_create();
  bai_wrapper_with_sim_results(&bai_options, rvs, rng, thread_control, NULL,
                               sim_results, bai_result);
  assert(bai_result_get_status(bai_result) == BAI_RESULT_STATUS_SAMPLE_LIMIT);
  assert(!bai_result_get_rule_zero_stopped(bai_result));
  assert(bai_result_get_rule_zero_would_stop(bai_result));
  assert(bai_result_get_rule_zero_stop_nodes(bai_result) >= 1);
  assert(bai_result_get_rule_zero_stop_iterations(bai_result) <
         bai_options.sample_limit);
  assert(bai_result_get_rule_zero_stable_checkpoints(bai_result) >= 2);
  assert(rvs_get_total_samples(rvs) == bai_options.sample_limit);

  bai_result_destroy(bai_result);
  thread_control_destroy(thread_control);
  sim_results_destroy(sim_results);
  rvs_destroy(rng);
  rvs_destroy(rvs);
}

static void bai_test_set_regret_arm(BAIArmDatum *arm, double mean,
                                    double sample_variance,
                                    uint64_t num_samples) {
  *arm = (BAIArmDatum){0};
  arm->mean = mean;
  arm->num_samples = num_samples;
  arm->var = sample_variance * (double)(num_samples - 1) / (double)num_samples;
}

static void test_bai_joint_max_regret_estimator(void) {
  BAIArmDatum two_arms[2];
  bai_test_set_regret_arm(&two_arms[0], 0.0, 1.0, 100);
  bai_test_set_regret_arm(&two_arms[1], -0.1, 1.0, 100);
  BAISyncData two_arm_sync = {
      .num_arms = 2,
      .astar_index = 0,
      .arm_data = two_arms,
      .regret_cross_arm_correlation = 0.4,
      .regret_calibration = 1.0,
      .regret_min_samples_per_arm = 32,
  };
  const double pairwise = bai_estimate_expected_regret(&two_arm_sync, NULL);
  const double joint = bai_estimate_joint_expected_regret(&two_arm_sync, NULL);
  // With one challenger, E[max(U0,U1)] - E[U0] is exactly the pairwise
  // positive-part expectation.
  assert(fabs(pairwise - joint) < 1e-12);
  assert(bai_count_near_tie_challengers(&two_arm_sync) == 1);

  BAIArmDatum flat_arms[6];
  for (int arm_index = 0; arm_index < 6; arm_index++) {
    bai_test_set_regret_arm(&flat_arms[arm_index], 0.0, 1.0, 100);
  }
  BAISyncData flat_sync = {
      .num_arms = 6,
      .astar_index = 0,
      .arm_data = flat_arms,
      .regret_cross_arm_correlation = 0.0,
      .regret_calibration = 1.0,
      .regret_min_samples_per_arm = 32,
  };
  const double flat_pairwise = bai_estimate_expected_regret(&flat_sync, NULL);
  const double flat_joint =
      bai_estimate_joint_expected_regret(&flat_sync, NULL);
  // The legacy maximum of pairwise regrets ignores that any of several tied
  // challengers can win. The joint maximum must expose that multiplicity.
  assert(flat_joint > flat_pairwise * 1.5);
  assert(bai_count_near_tie_challengers(&flat_sync) == 5);
  assert(bai_regret_stop_estimate(&flat_sync, flat_pairwise, flat_joint) ==
         flat_pairwise);
  flat_sync.regret_stop_use_joint = true;
  assert(bai_regret_stop_estimate(&flat_sync, flat_pairwise, flat_joint) ==
         flat_joint);

  BAIArmDatum separated_arms[3];
  bai_test_set_regret_arm(&separated_arms[0], 1.0, 0.01, 100);
  bai_test_set_regret_arm(&separated_arms[1], 0.0, 0.01, 100);
  bai_test_set_regret_arm(&separated_arms[2], -1.0, 0.01, 100);
  BAISyncData separated_sync = {
      .num_arms = 3,
      .astar_index = 0,
      .arm_data = separated_arms,
      .regret_cross_arm_correlation = 0.4,
      .regret_calibration = 1.0,
      .regret_min_samples_per_arm = 32,
  };
  const double separated_joint =
      bai_estimate_joint_expected_regret(&separated_sync, NULL);
  assert(isfinite(separated_joint));
  assert(separated_joint < 1e-9);
  assert(bai_count_near_tie_challengers(&separated_sync) == 0);
}

// A finite regret estimate is a statistical claim, not merely a formatting
// convenience. In particular, one draw gives an arm empirical variance zero;
// the numerical variance floor must not turn that into apparent certainty.
static void test_bai_regret_requires_minimum_arm_evidence(int num_threads) {
  const double means_and_vars[] = {0.6, 0.04, 0.5, 0.04};
  const uint64_t num_rvs = (sizeof(means_and_vars)) / (sizeof(double) * 2);
  RandomVariablesArgs rv_args = {
      .type = RANDOM_VARIABLES_NORMAL,
      .num_rvs = num_rvs,
      .means_and_vars = means_and_vars,
      .seed = 9182,
  };
  RandomVariablesArgs rng_args = {
      .type = RANDOM_VARIABLES_UNIFORM,
      .num_rvs = num_rvs,
      .seed = 9182,
  };
  RandomVariables *rvs = rvs_create(&rv_args);
  RandomVariables *rng = rvs_create(&rng_args);
  ThreadControl *thread_control = thread_control_create();
  BAIResult *bai_result = bai_result_create();
  BAIOptions bai_options = {
      .sampling_rule = BAI_SAMPLING_RULE_TOP_TWO_IDS,
      .threshold = BAI_THRESHOLD_NONE,
      .delta = 0.05,
      .sample_minimum = 1,
      .sample_limit = num_rvs,
      .time_limit_seconds = 0,
      .num_threads = num_threads,
      .cutoff = 0,
  };

  bai_wrapper(&bai_options, rvs, rng, thread_control, NULL, bai_result);
  assert(isinf(bai_result_get_estimated_regret(bai_result)));

  rvs_reset(rvs, &rv_args);
  bai_options.sample_minimum = BAI_MINIMUM_REGRET_SAMPLES_PER_ARM;
  bai_options.sample_limit = num_rvs * BAI_MINIMUM_REGRET_SAMPLES_PER_ARM;
  bai_wrapper(&bai_options, rvs, rng, thread_control, NULL, bai_result);
  const double estimated_regret = bai_result_get_estimated_regret(bai_result);
  assert(isfinite(estimated_regret));
  assert(estimated_regret >= 0.0);

  bai_result_destroy(bai_result);
  thread_control_destroy(thread_control);
  rvs_destroy(rng);
  rvs_destroy(rvs);
}

void test_bai_win_pct_cutoff_helper(int num_threads,
                                    const double *means_and_vars,
                                    const uint64_t num_rvs) {
  RandomVariablesArgs rv_args = {
      .type = RANDOM_VARIABLES_NORMAL,
      .num_rvs = num_rvs,
      .means_and_vars = means_and_vars,
      .seed = 10,
  };
  RandomVariables *rvs = rvs_create(&rv_args);

  RandomVariablesArgs rng_args = {
      .type = RANDOM_VARIABLES_UNIFORM,
      .num_rvs = num_rvs,
      .seed = 10,
  };
  RandomVariables *rng = rvs_create(&rng_args);

  BAIOptions bai_options = {
      .threshold = BAI_THRESHOLD_NONE,
      .delta = 0.05,
      .sample_minimum = 37,
      .sample_limit = 2000,
      .time_limit_seconds = 0,
      .num_threads = num_threads,
      .cutoff = 0.0005,
  };
  ThreadControl *thread_control = thread_control_create();
  BAIResult *bai_result = bai_result_create();
  for (int i = 0; i < num_sampling_rules; i++) {
    bai_options.sampling_rule = sampling_rules[i];
    rvs_reset(rvs, &rv_args);
    bai_wrapper(&bai_options, rvs, rng, thread_control, NULL, bai_result);
    assert(bai_result_get_status(bai_result) ==
           BAI_RESULT_STATUS_WIN_PCT_CUTOFF);
    assert(bai_result_get_best_arm(bai_result) == 1);
    uint64_t expected_num_samples = num_rvs * bai_options.sample_minimum;
    assert(rvs_get_total_samples(rvs) == expected_num_samples);
  }
  thread_control_destroy(thread_control);
  // The timer should stop once the BAI has finished.
  const double bai_time_elapsed = bai_result_get_elapsed_seconds(bai_result);
  ctime_nap(0.2);
  assert(bai_time_elapsed == bai_result_get_elapsed_seconds(bai_result));
  bai_result_destroy(bai_result);
  rvs_destroy(rng);
  rvs_destroy(rvs);
}

void test_bai_win_pct_cutoff(int num_threads) {
  const double means_and_vars1[] = {0.2, 1, 1, 0.001, 0.3, 1};
  const uint64_t num_rvs1 = (sizeof(means_and_vars1)) / (sizeof(double) * 2);
  test_bai_win_pct_cutoff_helper(num_threads, means_and_vars1, num_rvs1);

  const double means_and_vars2[] = {
      0, 0.0000001, 0.00001, 0.000001, 0, 0.0000001,
  };
  const uint64_t num_rvs2 = (sizeof(means_and_vars2)) / (sizeof(double) * 2);
  test_bai_win_pct_cutoff_helper(num_threads, means_and_vars2, num_rvs2);
}

typedef struct BAITestArgs {
  BAIOptions *options;
  RandomVariables *rvs;
  RandomVariables *rng;
  ThreadControl *thread_control;
  BAIResult *result;
  cpthread_mutex_t *mutex;
  cpthread_cond_t *cond;
  int *done;
} BAITestArgs;

void *bai_thread_func(void *arg) {
  BAITestArgs *args = (BAITestArgs *)arg;
  bai_wrapper(args->options, args->rvs, args->rng, args->thread_control, NULL,
              args->result);

  cpthread_mutex_lock(args->mutex);
  *(args->done) = 1;
  cpthread_cond_signal(args->cond);
  cpthread_mutex_unlock(args->mutex);

  return NULL;
}

void test_bai_time_limit(int num_threads) {
  const double means_and_vars[] = {0.1, 1, 0.5, 1, 0.2, 1};
  const int num_rvs = (sizeof(means_and_vars)) / (sizeof(double) * 2);
  RandomVariablesArgs rv_args = {
      .type = RANDOM_VARIABLES_NORMAL,
      .num_rvs = num_rvs,
      .means_and_vars = means_and_vars,
      .seed = 10,
  };
  RandomVariables *rvs = rvs_create(&rv_args);

  RandomVariablesArgs rng_args = {
      .type = RANDOM_VARIABLES_UNIFORM,
      .num_rvs = num_rvs,
      .seed = 10,
  };
  RandomVariables *rng = rvs_create(&rng_args);

  BAIOptions bai_options = {
      .sampling_rule = BAI_SAMPLING_RULE_TOP_TWO_IDS,
      .threshold = BAI_THRESHOLD_NONE,
      .delta = 0.01,
      .sample_minimum = 50,
      .sample_limit = 100000000,
      .time_limit_seconds = 2,
      .num_threads = num_threads,
      .cutoff = 0,
  };

  ThreadControl *thread_control = thread_control_create();
  BAIResult *bai_result = bai_result_create();
  int done = 0;

  cpthread_mutex_t mutex;
  cpthread_mutex_init(&mutex);
  cpthread_cond_t cond;
  cpthread_cond_init(&cond);

  BAITestArgs args = {.options = &bai_options,
                      .rvs = rvs,
                      .rng = rng,
                      .thread_control = thread_control,
                      .result = bai_result,
                      .mutex = &mutex,
                      .cond = &cond,
                      .done = &done};

  cpthread_t thread;
  cpthread_create(&thread, bai_thread_func, &args);
  cpthread_cond_timedwait_loop(&cond, &mutex, 10, &done);
  cpthread_join(thread);

  assert(bai_result_get_status(bai_result) == BAI_RESULT_STATUS_TIMEOUT);

  bai_result_destroy(bai_result);
  thread_control_destroy(thread_control);
  rvs_destroy(rng);
  rvs_destroy(rvs);
}

void test_bai_interrupt(int num_threads) {
  const double means_and_vars[] = {0.1, 1, 0.5, 1, 0.2, 1, 0.25, 1};
  const int num_rvs = (sizeof(means_and_vars)) / (sizeof(double) * 2);
  RandomVariablesArgs rv_args = {
      .type = RANDOM_VARIABLES_NORMAL,
      .num_rvs = num_rvs,
      .means_and_vars = means_and_vars,
      .seed = 10,
  };
  RandomVariables *rvs = rvs_create(&rv_args);

  RandomVariablesArgs rng_args = {
      .type = RANDOM_VARIABLES_UNIFORM,
      .num_rvs = num_rvs,
      .seed = 10,
  };
  RandomVariables *rng = rvs_create(&rng_args);

  BAIOptions bai_options = {
      .sampling_rule = BAI_SAMPLING_RULE_TOP_TWO_IDS,
      .threshold = BAI_THRESHOLD_NONE,
      .delta = 0.01,
      .sample_minimum = 50,
      .sample_limit = 100000000,
      .time_limit_seconds = 20,
      .num_threads = num_threads,
      .cutoff = 0,
  };

  ThreadControl *thread_control = thread_control_create();
  BAIResult *bai_result = bai_result_create();
  int done = 0;

  cpthread_mutex_t mutex;
  cpthread_mutex_init(&mutex);
  cpthread_cond_t cond;
  cpthread_cond_init(&cond);

  BAITestArgs args = {.options = &bai_options,
                      .rvs = rvs,
                      .rng = rng,
                      .thread_control = thread_control,
                      .result = bai_result,
                      .mutex = &mutex,
                      .cond = &cond,
                      .done = &done};

  cpthread_t thread;
  cpthread_create(&thread, bai_thread_func, &args);
  ctime_nap(2.0);
  thread_control_set_status(thread_control,
                            THREAD_CONTROL_STATUS_USER_INTERRUPT);
  cpthread_cond_timedwait_loop(&cond, &mutex, 5, &done);
  cpthread_join(thread);

  assert(bai_result_get_status(bai_result) == BAI_RESULT_STATUS_USER_INTERRUPT);

  bai_result_destroy(bai_result);
  thread_control_destroy(thread_control);
  rvs_destroy(rng);
  rvs_destroy(rvs);
}

// Assumes rv_args are normal predetermined
// Assumes rng_args are uniform
void write_bai_input(const double delta, const RandomVariablesArgs *rv_args,
                     const RandomVariablesArgs *rng_args) {
  FILE *file = fopen_or_die("normal_data.txt", "w");
  fprintf_or_die(file, "%0.20f\n", delta);
  fprintf_or_die(file, "%" PRIu64 "\n", rv_args->num_rvs);
  for (uint64_t i = 0; i < rv_args->num_rvs; i++) {
    fprintf_or_die(file, "%0.20f,%0.20f\n", rv_args->means_and_vars[i * 2],
                   rv_args->means_and_vars[i * 2 + 1]);
  }
  fprintf_or_die(file, "%" PRIu64 "\n", rv_args->num_samples);
  for (uint64_t i = 0; i < rv_args->num_samples; i++) {
    fprintf_or_die(file, "%0.20f\n", rv_args->samples[i]);
  }
  RandomVariables *rng = rvs_create(rng_args);
  for (uint64_t i = 0; i < rv_args->num_samples; i++) {
    fprintf_or_die(file, "%0.20f\n", rvs_sample(rng, 0, 0, NULL));
  }
  rvs_destroy(rng);
  fclose_or_die(file);
}

void test_bai_similarity(int num_threads) {
  const int num_samples = 1000;
  double *samples = (double *)malloc_or_die(num_samples * sizeof(double));
  for (int i = 0; i < num_samples; i++) {
    samples[i] = 0.5;
  }
  RandomVariablesArgs rv_args = {
      .type = RANDOM_VARIABLES_NORMAL_PREDETERMINED,
      .num_samples = num_samples,
      .samples = samples,
  };
  RandomVariablesArgs rng_args = {
      .type = RANDOM_VARIABLES_UNIFORM,
      .seed = 10,
  };
  BAIOptions bai_options = {
      .delta = 0.01,
      .sample_minimum = 50,
      .sample_limit = num_samples,
      .time_limit_seconds = 0,
      .num_threads = num_threads,
      .cutoff = 0,
  };

  ThreadControl *thread_control = thread_control_create();
  BAIResult *bai_result = bai_result_create();

  for (int max_classes = 1; max_classes <= 3; max_classes++) {
    for (int num_rvs = 2; num_rvs <= 10; num_rvs++) {
      double *means_and_vars =
          (double *)malloc_or_die((size_t)num_rvs * 2 * sizeof(double));
      for (int i = 0; i < num_rvs; i++) {
        means_and_vars[(ptrdiff_t)(i * 2)] =
            0.03 * (max_classes - (i % max_classes));
        means_and_vars[(ptrdiff_t)(i * 2 + 1)] =
            0.05 * (max_classes - (i % max_classes));
      }
      rv_args.num_rvs = num_rvs;
      rv_args.means_and_vars = means_and_vars;
      rng_args.num_rvs = num_rvs;
      for (int i = 0; i < num_strategies_entries; i++) {
        RandomVariables *rvs = rvs_create(&rv_args);
        RandomVariables *rng = rvs_create(&rng_args);
        BAILogger *bai_logger = NULL;
        bai_options.sampling_rule = strategies[i][0];
        bai_options.threshold = strategies[i][1];
        bai_wrapper(&bai_options, rvs, rng, thread_control, bai_logger,
                    bai_result);
        bai_logger_flush(bai_logger);
        bai_logger_destroy(bai_logger);
        assert(bai_result_get_best_arm(bai_result) % max_classes == 0);
        assert(bai_result_get_status(bai_result) ==
               BAI_RESULT_STATUS_THRESHOLD);
        rvs_destroy(rvs);
        rvs_destroy(rng);
      }
      free(means_and_vars);
    }
  }
  bai_result_destroy(bai_result);
  free(samples);
  thread_control_destroy(thread_control);
}

void test_bai_from_seed(const char *bai_seed) {
  ErrorStack *error_stack = error_stack_create();
  const uint64_t seed = string_to_uint64(bai_seed, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("invalid seed: %s\n", bai_seed);
  }
  error_stack_destroy(error_stack);
  printf("running bai comparison with seed %s\n", bai_seed);

  XoshiroPRNG *prng = prng_create(seed);

  const uint64_t num_rvs =
      prng_get_random_number(prng, (uint64_t)20) + (uint64_t)2;
  const uint64_t rv_seed = prng_get_random_number(prng, UINT64_MAX);
  const uint64_t rng_seed = prng_get_random_number(prng, UINT64_MAX);

  double *means_and_vars = malloc_or_die(num_rvs * 2 * sizeof(double));
  int means_map[NUM_UNIQUE_MEANS];
  for (int i = 0; i < NUM_UNIQUE_MEANS; i++) {
    means_map[i] = 0;
  }
  for (uint64_t i = 0; i < num_rvs * 2; i++) {
    double value;
    if (i % 2 == 1) {
      value = (double)(prng_get_random_number(prng, 10) + 1);
    } else {
      int mean_int = (int)prng_get_random_number(prng, NUM_UNIQUE_MEANS);
      while (means_map[mean_int] != 0) {
        mean_int = (int)prng_get_random_number(prng, NUM_UNIQUE_MEANS);
      }
      means_map[mean_int] = 1;
      value = (mean_int - (double)(NUM_UNIQUE_MEANS) / 2.0) / 100.0;
    }
    means_and_vars[i] = value;
  }

  prng_destroy(prng);

  RandomVariablesArgs rv_args = {
      .type = RANDOM_VARIABLES_NORMAL,
      .num_rvs = num_rvs,
      .seed = rv_seed,
      .means_and_vars = means_and_vars,
  };
  RandomVariables *rvs = rvs_create(&rv_args);

  RandomVariablesArgs rng_args = {
      .type = RANDOM_VARIABLES_UNIFORM,
      .num_rvs = 1,
      .seed = rng_seed,
  };
  RandomVariables *rng = rvs_create(&rng_args);

  BAIOptions bai_options = {
      .sampling_rule = BAI_SAMPLING_RULE_TOP_TWO_IDS,
      .threshold = BAI_THRESHOLD_GK16,
      .delta = 0.01,
      .sample_minimum = 50,
      .sample_limit = 100000,
      .time_limit_seconds = 0,
      .num_threads = 1,
      .cutoff = 0,
  };
  ThreadControl *thread_control = thread_control_create();
  BAIResult *bai_result = bai_result_create();
  BAILogger *bai_logger = bai_logger_create("bai_log.txt");

  bai_wrapper(&bai_options, rvs, rng, thread_control, bai_logger, bai_result);

  bai_logger_log_int(bai_logger, "result",
                     bai_result_get_best_arm(bai_result) + 1);
  bai_logger_flush(bai_logger);

  bai_result_destroy(bai_result);
  bai_logger_destroy(bai_logger);
  thread_control_destroy(thread_control);
  rvs_destroy(rvs);
  rvs_destroy(rng);
  free(means_and_vars);
}

void test_bai(void) {
  const char *bai_seed = getenv("BAI_SEED");
  if (bai_seed) {
    test_bai_from_seed(bai_seed);
  } else {
    test_bai_joint_max_regret_estimator();
    const int num_threads[] = {1, 11};
    const int num_thread_tests = sizeof(num_threads) / sizeof(int);
    for (int i = 0; i < num_thread_tests; i++) {
      const int num_threads_i = num_threads[i];
      test_bai_sample_limit(num_threads_i);
      test_bai_regret_limit(num_threads_i);
      test_bai_rule_zero_stop(num_threads_i);
      test_bai_rule_zero_fails_closed_without_work_counter(num_threads_i);
      test_bai_rule_zero_requires_zero_near_ties(num_threads_i);
      test_bai_rule_zero_shadow_records_without_stopping(num_threads_i);
      test_bai_regret_requires_minimum_arm_evidence(num_threads_i);
      test_bai_win_pct_cutoff(num_threads_i);
      test_bai_time_limit(num_threads_i);
      test_bai_interrupt(num_threads_i);
      test_bai_top_two(num_threads_i);
      test_bai_similarity(num_threads_i);
    }
  }
}
