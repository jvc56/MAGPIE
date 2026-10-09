#include "bai_test.h"

#include "../src/compat/cpthread.h"
#include "../src/compat/ctime.h"
#include "../src/def/bai_defs.h"
#include "../src/def/cpthread_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/bai_result.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/xoshiro.h"
#include "../src/impl/bai.h"
#include "../src/impl/bai_logger.h"
#include "../src/impl/random_variable.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

void bai_wrapper(BAIOptions *bai_options, RandomVariables *rvs,
                 ThreadControl *thread_control, BAILogger *bai_logger,
                 BAIResult *bai_result) {
  bai_options->parent_worker_thread_index = 0;
  thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED);
  bai(bai_options, rvs, thread_control, bai_logger, bai_result);
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
  bai_wrapper(&bai_options, rvs, thread_control, NULL, bai_result);
  assert(bai_result_get_status(bai_result) == BAI_RESULT_STATUS_THRESHOLD);
  assert(bai_result_get_best_arm(bai_result) == 1);
  thread_control_destroy(thread_control);
  bai_result_destroy(bai_result);
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
    bai_wrapper(&bai_options, rvs, thread_control, NULL, bai_result);
    assert(bai_result_get_status(bai_result) == BAI_RESULT_STATUS_SAMPLE_LIMIT);
    assert(bai_result_get_best_arm(bai_result) == 1);
    uint64_t expected_num_samples = bai_options.sample_limit;
    if (expected_num_samples < num_rvs * bai_options.sample_minimum) {
      expected_num_samples = num_rvs * bai_options.sample_minimum;
    }
    assert(rvs_get_total_samples(rvs) == expected_num_samples);
    assert(bai_result_get_num_samples(bai_result) == expected_num_samples);
  }
  thread_control_destroy(thread_control);
  // The timer should stop once the BAI has finished.
  const double bai_time_elapsed = bai_result_get_elapsed_seconds(bai_result);
  ctime_nap(0.2);
  assert(bai_time_elapsed == bai_result_get_elapsed_seconds(bai_result));
  bai_result_destroy(bai_result);
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
    bai_wrapper(&bai_options, rvs, thread_control, NULL, bai_result);
    assert(bai_result_get_status(bai_result) ==
           BAI_RESULT_STATUS_WIN_PCT_CUTOFF);
    assert(bai_result_get_best_arm(bai_result) == 1);
    // The cutoff stops the sim as soon as the initial phase has folded, so
    // the results hold exactly the initial phase. With several threads,
    // later rounds may already be in flight when that fold runs; they are
    // computed but discarded, so only the committed count is exact.
    const uint64_t expected_num_samples = num_rvs * bai_options.sample_minimum;
    assert(bai_result_get_num_samples(bai_result) == expected_num_samples);
    assert(rvs_get_total_samples(rvs) >= expected_num_samples);
    if (num_threads == 1) {
      assert(rvs_get_total_samples(rvs) == expected_num_samples);
    }
  }
  thread_control_destroy(thread_control);
  // The timer should stop once the BAI has finished.
  const double bai_time_elapsed = bai_result_get_elapsed_seconds(bai_result);
  ctime_nap(0.2);
  assert(bai_time_elapsed == bai_result_get_elapsed_seconds(bai_result));
  bai_result_destroy(bai_result);
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
  ThreadControl *thread_control;
  BAIResult *result;
  cpthread_mutex_t *mutex;
  cpthread_cond_t *cond;
  int *done;
} BAITestArgs;

void *bai_thread_func(void *arg) {
  BAITestArgs *args = (BAITestArgs *)arg;
  bai_wrapper(args->options, args->rvs, args->thread_control, NULL,
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
  rvs_destroy(rvs);
}

// A round-robin claim holds a batch of slots. Once the clock has run out, a
// worker stops after the slot it is on instead of running the rest of the
// batch past the deadline. Drives the scheduler single-threaded, so it does
// not depend on timing.
void test_bai_claim_stops_within_one_sample(void) {
  const double means_and_vars[] = {0.1, 1, 0.5, 1, 0.2, 1};
  const int num_rvs = (sizeof(means_and_vars)) / (sizeof(double) * 2);
  RandomVariablesArgs rv_args = {
      .type = RANDOM_VARIABLES_NORMAL,
      .num_rvs = num_rvs,
      .means_and_vars = means_and_vars,
      .seed = 10,
  };
  RandomVariables *rvs = rvs_create(&rv_args);
  BAIOptions bai_options = {
      .sampling_rule = BAI_SAMPLING_RULE_ROUND_ROBIN,
      .threshold = BAI_THRESHOLD_NONE,
      .delta = 0.01,
      .sample_minimum = 50,
      .sample_limit = 1000,
      .num_threads = 1,
  };
  ThreadControl *thread_control = thread_control_create();
  BAIResult *bai_result = bai_result_create();
  bai_result_reset(bai_result, 0);
  BAISyncData *sync_data =
      bai_sync_data_create(bai_result, thread_control, num_rvs);
  sync_data->record_size = rvs_get_sample_record_size(rvs);
  BAISampleArgs sample_args =
      bai_sample_args_create(sync_data, rvs, &bai_options);
  bai_sync_data_start_schedule(&sample_args, bai_options.sample_minimum);
  const BAIWorkerArgs worker_args = {
      .sync_data = sync_data,
      .rvs = rvs,
      .bai_options = &bai_options,
      .thread_index = 0,
  };
  BAIWorkerSchedStats worker_stats;
  memset(&worker_stats, 0, sizeof(worker_stats));
  double sample_values[BAI_SCHEDULE_ROUND_ROBIN_CLAIM_BATCH];

  // With no time limit the whole claim is sampled.
  BAIClaim claim;
  bool claimed = bai_schedule_claim_while_locked(sync_data, &claim);
  assert(claimed);
  assert(claim.num_slots == BAI_SCHEDULE_ROUND_ROBIN_CLAIM_BATCH);
  bool abandoned = false;
  int num_sampled = bai_worker_sample_claim(&worker_stats, &worker_args, &claim,
                                            sample_values, &abandoned);
  assert(num_sampled == claim.num_slots);
  assert(!abandoned);

  // Once the clock has run out, only the slot already started is sampled.
  bai_result_reset(bai_result, 1e-9);
  ctime_nap(0.01);
  claimed = bai_schedule_claim_while_locked(sync_data, &claim);
  assert(claimed);
  assert(claim.num_slots == BAI_SCHEDULE_ROUND_ROBIN_CLAIM_BATCH);
  (void)claimed;
  num_sampled = bai_worker_sample_claim(&worker_stats, &worker_args, &claim,
                                        sample_values, &abandoned);
  assert(num_sampled == 1);
  assert(abandoned);
  (void)num_sampled;

  bai_sync_data_destroy(sync_data);
  bai_result_destroy(bai_result);
  thread_control_destroy(thread_control);
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
  rvs_destroy(rvs);
}

// A sim cut short by the clock or an interrupt folds every sample that has
// finished, even when no round is complete, and discards any that finish
// later. Drives the scheduler single-threaded, so it does not depend on
// timing.
void test_bai_abandon_folds_finished_samples(void) {
  const double means_and_vars[] = {0.1, 1, 0.5, 1, 0.2, 1};
  const int num_rvs = (sizeof(means_and_vars)) / (sizeof(double) * 2);
  RandomVariablesArgs rv_args = {
      .type = RANDOM_VARIABLES_NORMAL,
      .num_rvs = num_rvs,
      .means_and_vars = means_and_vars,
      .seed = 10,
  };
  RandomVariables *rvs = rvs_create(&rv_args);
  BAIOptions bai_options = {
      .sampling_rule = BAI_SAMPLING_RULE_TOP_TWO_IDS,
      .threshold = BAI_THRESHOLD_NONE,
      .delta = 0.01,
      .sample_minimum = 50,
      .sample_limit = 1000,
      .num_threads = 1,
  };
  ThreadControl *thread_control = thread_control_create();
  BAIResult *bai_result = bai_result_create();
  bai_result_reset(bai_result, 0);
  BAISyncData *sync_data =
      bai_sync_data_create(bai_result, thread_control, num_rvs);
  sync_data->record_size = rvs_get_sample_record_size(rvs);
  BAISampleArgs sample_args =
      bai_sample_args_create(sync_data, rvs, &bai_options);
  bai_sync_data_start_schedule(&sample_args, bai_options.sample_minimum);

  // Claim a batch of the first round and finish only some of it; the rest
  // stays in flight, so the round is incomplete and nothing has folded.
  BAIClaim claim;
  const bool claimed = bai_schedule_claim_while_locked(sync_data, &claim);
  assert(claimed);
  (void)claimed;
  assert(claim.round_number == 0);
  assert(claim.num_slots > 3);
  const int num_finished = 3;
  const BAIRound *round = &sync_data->rounds[0];
  for (int slot = 0; slot < num_finished; slot++) {
    const double sample =
        rvs_sample_with_seed(rvs, (uint64_t)round->arm_indices[slot],
                             round->seeds[slot], 0, NULL, NULL);
    bai_schedule_complete_while_locked(&sample_args, 0, slot, sample);
  }
  assert(sync_data->num_total_samples_completed == 0);
  assert(sync_data->astar_index == -1);

  bai_abandon_rounds_while_locked(&sample_args);
  assert(sync_data->stopped);
  assert(sync_data->num_total_samples_completed == (uint64_t)num_finished);
  // The first round is round-robin, so each arm got one of the samples.
  for (int arm_index = 0; arm_index < num_rvs; arm_index++) {
    assert(sync_data->arm_data[arm_index].num_samples == 1);
  }
  assert(sync_data->astar_index >= 0 && sync_data->astar_index < num_rvs);

  // A sample finishing after the abandon is discarded, and a second abandon
  // changes nothing.
  bai_schedule_complete_while_locked(&sample_args, 0, num_finished, 0.5);
  bai_abandon_rounds_while_locked(&sample_args);
  assert(sync_data->num_total_samples_completed == (uint64_t)num_finished);

  bai_sync_data_destroy(sync_data);
  bai_result_destroy(bai_result);
  thread_control_destroy(thread_control);
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
      for (int i = 0; i < num_strategies_entries; i++) {
        RandomVariables *rvs = rvs_create(&rv_args);
        BAILogger *bai_logger = NULL;
        bai_options.sampling_rule = strategies[i][0];
        bai_options.threshold = strategies[i][1];
        bai_wrapper(&bai_options, rvs, thread_control, bai_logger, bai_result);
        bai_logger_flush(bai_logger);
        bai_logger_destroy(bai_logger);
        assert(bai_result_get_best_arm(bai_result) % max_classes == 0);
        assert(bai_result_get_status(bai_result) ==
               BAI_RESULT_STATUS_THRESHOLD);
        rvs_destroy(rvs);
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
  // Formerly the seed of BAI's coin; still drawn so that a given BAI_SEED
  // keeps producing the same means.
  prng_get_random_number(prng, UINT64_MAX);

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

  bai_wrapper(&bai_options, rvs, thread_control, bai_logger, bai_result);

  bai_logger_log_int(bai_logger, "result",
                     bai_result_get_best_arm(bai_result) + 1);
  bai_logger_flush(bai_logger);

  bai_result_destroy(bai_result);
  bai_logger_destroy(bai_logger);
  thread_control_destroy(thread_control);
  rvs_destroy(rvs);
  free(means_and_vars);
}

void test_bai(void) {
  const char *bai_seed = getenv("BAI_SEED");
  if (bai_seed) {
    test_bai_from_seed(bai_seed);
  } else {
    test_bai_abandon_folds_finished_samples();
    test_bai_claim_stops_within_one_sample();
    const int num_threads[] = {1, 11};
    const int num_thread_tests = sizeof(num_threads) / sizeof(int);
    for (int i = 0; i < num_thread_tests; i++) {
      const int num_threads_i = num_threads[i];
      test_bai_sample_limit(num_threads_i);
      test_bai_win_pct_cutoff(num_threads_i);
      test_bai_time_limit(num_threads_i);
      test_bai_interrupt(num_threads_i);
      test_bai_top_two(num_threads_i);
      test_bai_similarity(num_threads_i);
    }
  }
}
