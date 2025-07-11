#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "../../src/def/bai_defs.h"

#include "../../src/impl/bai_logger.h"
#include "../../src/impl/random_variable.h"

#include "../../src/impl/bai.h"
#include "../../src/util/io_util.h"

#define NUM_UNIQUE_MEANS 10000

static const int sampling_rules[3] = {
    BAI_SAMPLING_RULE_ROUND_ROBIN,
    BAI_SAMPLING_RULE_TOP_TWO,
};
static const int num_sampling_rules = sizeof(sampling_rules) / sizeof(int);

static const int strategies[][3] = {
    {BAI_SAMPLING_RULE_TOP_TWO, BAI_THRESHOLD_GK16},
};
static const int num_strategies_entries =
    sizeof(strategies) / sizeof(strategies[0]);

void assert_num_epigons(const RandomVariables *rvs,
                        const int expected_num_epigons) {
  const int num_rvs = rvs_get_num_rvs(rvs);
  int actual_num_epigons = 0;
  for (int k = 0; k < num_rvs; k++) {
    if (rvs_is_epigon(rvs, k)) {
      actual_num_epigons++;
    }
  }
  assert(expected_num_epigons == actual_num_epigons);
}

void test_bai_top_two(int num_threads) {
  const double means_and_vars[] = {-10, 1, 0, 1};
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
      .sampling_rule = BAI_SAMPLING_RULE_TOP_TWO,
      .threshold = BAI_THRESHOLD_GK16,
      .delta = 0.05,
      .sample_limit = 200,
      .time_limit_seconds = 0,
  };

  ThreadControl *thread_control = thread_control_create();
  thread_control_set_threads(thread_control, num_threads);
  BAIResult *bai_result = bai_result_create();
  bai(&bai_options, rvs, rng, thread_control, NULL, bai_result);
  assert(bai_result_get_exit_status(bai_result) == EXIT_STATUS_THRESHOLD);
  assert(bai_result_get_best_arm(bai_result) == 1);
  thread_control_destroy(thread_control);
  bai_result_destroy(bai_result);
  rvs_destroy(rng);
  rvs_destroy(rvs);
}

void test_bai_sample_limit(int num_threads) {
  const double means_and_vars[] = {-10, 1, 100, 10, -20, 5};
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
      .threshold = BAI_THRESHOLD_NONE,
      .delta = 0.05,
      .sample_limit = 200,
      .time_limit_seconds = 0,
  };
  ThreadControl *thread_control = thread_control_create();
  thread_control_set_threads(thread_control, num_threads);
  BAIResult *bai_result = bai_result_create();
  // This needs to align with the ARM_SAMPLE_MINIMUM in bai.c
  const int arm_sample_minimum = 50;
  for (int i = 0; i < num_sampling_rules; i++) {
    bai_options.sampling_rule = sampling_rules[i];
    bai(&bai_options, rvs, rng, thread_control, NULL, bai_result);
    assert(bai_result_get_exit_status(bai_result) == EXIT_STATUS_SAMPLE_LIMIT);
    assert(bai_result_get_best_arm(bai_result) == 1);
    int expected_num_samples = bai_options.sample_limit;
    if (expected_num_samples < num_rvs * arm_sample_minimum) {
      expected_num_samples = num_rvs * arm_sample_minimum;
    }
    assert(rvs_get_total_samples(rvs) == (uint64_t)expected_num_samples);
    assert(bai_result_get_total_samples(bai_result) == expected_num_samples);
    assert_num_epigons(rvs, 0);
  }
  thread_control_destroy(thread_control);
  bai_result_destroy(bai_result);
  rvs_destroy(rng);
  rvs_destroy(rvs);
}

typedef struct BAITestArgs {
  const BAIOptions *options;
  RandomVariables *rvs;
  RandomVariables *rng;
  ThreadControl *thread_control;
  BAIResult *result;
  pthread_mutex_t *mutex;
  pthread_cond_t *cond;
  int *done;
} BAITestArgs;

void *bai_thread_func(void *arg) {
  BAITestArgs *args = (BAITestArgs *)arg;
  bai(args->options, args->rvs, args->rng, args->thread_control, NULL,
      args->result);

  pthread_mutex_lock(args->mutex);
  *(args->done) = 1;
  pthread_cond_signal(args->cond);
  pthread_mutex_unlock(args->mutex);

  return NULL;
}

void test_bai_time_limit(int num_threads) {
  const double means_and_vars[] = {-10, 1, 0, 1, 100, 10, -20, 5};
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
      .sampling_rule = BAI_SAMPLING_RULE_TOP_TWO,
      .threshold = BAI_THRESHOLD_NONE,
      .delta = 0.01,
      .sample_limit = 100000000,
      .time_limit_seconds = 2,
  };

  ThreadControl *thread_control = thread_control_create();
  thread_control_set_threads(thread_control, num_threads);
  BAIResult *bai_result = bai_result_create();
  int done = 0;

  pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

  BAITestArgs args = {.options = &bai_options,
                      .rvs = rvs,
                      .rng = rng,
                      .thread_control = thread_control,
                      .result = bai_result,
                      .mutex = &mutex,
                      .cond = &cond,
                      .done = &done};

  pthread_t thread;
  pthread_create(&thread, NULL, bai_thread_func, &args);

  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  const int timeout_seconds = bai_options.time_limit_seconds + 5;
  ts.tv_sec += timeout_seconds;

  pthread_mutex_lock(&mutex);
  while (!done) {
    int ret = pthread_cond_timedwait(&cond, &mutex, &ts);
    if (ret == ETIMEDOUT) {
      printf("bai did not complete within %d seconds.\n", timeout_seconds);
      pthread_cancel(thread);
      pthread_mutex_unlock(&mutex);
      assert(0);
    }
  }
  pthread_mutex_unlock(&mutex);

  pthread_join(thread, NULL);

  assert(bai_result_get_exit_status(bai_result) == EXIT_STATUS_TIMEOUT);

  bai_result_destroy(bai_result);
  thread_control_destroy(thread_control);
  rvs_destroy(rng);
  rvs_destroy(rvs);
}

// Assumes rv_args are normal predetermined
// Assumes rng_args are uniform
void write_bai_input(const double delta, const RandomVariablesArgs *rv_args,
                     const RandomVariablesArgs *rng_args) {
  FILE *file = fopen("normal_data.txt", "w");
  fprintf(file, "%0.20f\n", delta);
  fprintf(file, "%" PRIu64 "\n", rv_args->num_rvs);
  for (uint64_t i = 0; i < rv_args->num_rvs; i++) {
    fprintf(file, "%0.20f,%0.20f\n", rv_args->means_and_vars[i * 2],
            rv_args->means_and_vars[i * 2 + 1]);
  }
  fprintf(file, "%" PRIu64 "\n", rv_args->num_samples);
  for (uint64_t i = 0; i < rv_args->num_samples; i++) {
    fprintf(file, "%0.20f\n", rv_args->samples[i]);
  }
  RandomVariables *rng = rvs_create(rng_args);
  for (uint64_t i = 0; i < rv_args->num_samples; i++) {
    fprintf(file, "%0.20f\n", rvs_sample(rng, 0, 0, NULL));
  }
  rvs_destroy(rng);
  fclose_or_die(file);
}

void test_bai_epigons(int num_threads) {
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
      .sample_limit = num_samples,
      .time_limit_seconds = 0,
  };

  ThreadControl *thread_control = thread_control_create();
  thread_control_set_threads(thread_control, num_threads);
  BAIResult *bai_result = bai_result_create();

  for (int max_classes = 1; max_classes <= 3; max_classes++) {
    exit_status_t expected_exit_status = EXIT_STATUS_THRESHOLD;
    if (max_classes == 1) {
      expected_exit_status = EXIT_STATUS_ONE_ARM_REMAINING;
    }
    for (int num_rvs = 2; num_rvs <= 10; num_rvs++) {
      double *means_and_vars =
          (double *)malloc_or_die(num_rvs * 2 * sizeof(double));
      for (int i = 0; i < num_rvs; i++) {
        means_and_vars[i * 2] = 3 * (max_classes - (i % max_classes));
        means_and_vars[i * 2 + 1] = 5 * (max_classes - (i % max_classes));
      }
      int expected_epigons = num_rvs - max_classes;
      if (expected_epigons < 0) {
        expected_epigons = 0;
      }
      rv_args.num_rvs = num_rvs;
      rv_args.means_and_vars = means_and_vars;
      rng_args.num_rvs = num_rvs;
      for (int i = 0; i < num_strategies_entries; i++) {
        RandomVariables *rvs = rvs_create(&rv_args);
        RandomVariables *rng = rvs_create(&rng_args);
        assert_num_epigons(rvs, 0);
        BAILogger *bai_logger = NULL;
        bai_options.sampling_rule = strategies[i][0];
        bai_options.threshold = strategies[i][1];
        bai(&bai_options, rvs, rng, thread_control, bai_logger, bai_result);
        bai_logger_flush(bai_logger);
        bai_logger_destroy(bai_logger);
        assert(bai_result_get_best_arm(bai_result) % max_classes == 0);
        assert(bai_result_get_exit_status(bai_result) == expected_exit_status);
        assert_num_epigons(rvs, expected_epigons);
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

void test_bai_input_from_file(const char *bai_input_filename,
                              const char *bai_params_index) {
  FILE *file = fopen(bai_input_filename, "r");
  if (!file) {
    log_fatal("Failed to open BAI_INPUT file: %s\n", bai_input_filename);
  }

  double delta;
  int num_rvs;
  double *means_and_vars;
  int num_samples;
  double *samples;
  double *rng_samples;

  if (fscanf(file, "%lf", &delta) != 1) {
    log_fatal("Failed to read delta from file: %s\n", bai_input_filename);
  }

  if (fscanf(file, "%d", &num_rvs) != 1) {
    log_fatal("Failed to read num_rvs from file: %s\n", bai_input_filename);
  }

  means_and_vars = (double *)malloc_or_die(
      num_rvs * 2 * sizeof(double)); // 2 values per RV (mean and std dev)
  for (int i = 0; i < num_rvs; ++i) {
    if (fscanf(file, "%lf,%lf", &means_and_vars[i * 2],
               &means_and_vars[i * 2 + 1]) != 2) {
      log_fatal("Failed to read means and stdevs from file at index %d.\n", i);
    }
  }
  if (fscanf(file, "%d", &num_samples) != 1) {
    log_fatal("Failed to read num_samples from file: %s\n", bai_input_filename);
  }

  samples = (double *)malloc_or_die(num_samples * sizeof(double));
  for (int i = 0; i < num_samples; ++i) {
    if (fscanf(file, "%lf", &samples[i]) != 1) {
      log_fatal("Failed to read sample %d from file.\n", i);
    }
  }

  rng_samples = (double *)malloc_or_die(num_samples * sizeof(double));
  for (int i = 0; i < num_samples; ++i) {
    if (fscanf(file, "%lf", &rng_samples[i]) != 1) {
      log_fatal("Failed to read sample %d from file.\n", i);
    }
  }

  fclose_or_die(file);

  BAILogger *bai_logger = bai_logger_create("bai_log_magpie.txt");

  bai_logger_log_double(bai_logger, "delta", delta);
  bai_logger_log_int(bai_logger, "num_rvs", num_rvs);
  bai_logger_log_double_array(bai_logger, "means_and_stddevs", means_and_vars,
                              num_rvs * 2);
  bai_logger_log_int(bai_logger, "num_samples", num_samples);
  bai_logger_flush(bai_logger);

  RandomVariablesArgs rv_args = {
      .type = RANDOM_VARIABLES_NORMAL_PREDETERMINED,
      .num_rvs = num_rvs,
      .means_and_vars = means_and_vars,
      .num_samples = num_samples,
      .samples = samples,
  };
  RandomVariables *rvs = rvs_create(&rv_args);

  RandomVariablesArgs rng_args = {
      .type = RANDOM_VARIABLES_UNIFORM_PREDETERMINED,
      .num_rvs = num_rvs,
      .num_samples = num_samples,
      .samples = rng_samples,
  };
  RandomVariables *rng = rvs_create(&rng_args);

  const int pi = atoi(bai_params_index);
  if (pi < 0 || pi >= num_strategies_entries) {
    log_fatal("Invalid BAI params index: %s\n", bai_params_index);
  }
  printf("magpie bai test running 0-indexed %d...\n", pi);

  BAIOptions bai_options = {
      .sampling_rule = strategies[pi][0],
      .threshold = strategies[pi][1],
      .delta = delta,
      .sample_limit = num_samples,
      .time_limit_seconds = 0,
  };
  ThreadControl *thread_control = thread_control_create();
  BAIResult *bai_result = bai_result_create();

  bai(&bai_options, rvs, rng, thread_control, bai_logger, bai_result);

  bai_logger_log_int(bai_logger, "result",
                     bai_result_get_best_arm(bai_result) + 1);
  bai_logger_flush(bai_logger);

  bai_result_destroy(bai_result);
  bai_logger_destroy(bai_logger);
  thread_control_destroy(thread_control);
  rvs_destroy(rvs);
  rvs_destroy(rng);
  free(means_and_vars);
  free(samples);
  free(rng_samples);
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

  const int num_rvs = prng_get_random_number(prng, 20) + 2;
  const uint64_t rv_seed = prng_get_random_number(prng, UINT64_MAX);
  const uint64_t rng_seed = prng_get_random_number(prng, UINT64_MAX);

  double *means_and_vars = malloc_or_die(num_rvs * 2 * sizeof(double));
  int means_map[NUM_UNIQUE_MEANS];
  for (int i = 0; i < NUM_UNIQUE_MEANS; i++) {
    means_map[i] = 0;
  }
  for (int i = 0; i < num_rvs * 2; i++) {
    double value;
    if (i % 2 == 1) {
      value = (double)(prng_get_random_number(prng, 10) + 1);
    } else {
      int mean_int = prng_get_random_number(prng, NUM_UNIQUE_MEANS);
      while (means_map[mean_int] != 0) {
        mean_int = prng_get_random_number(prng, NUM_UNIQUE_MEANS);
      }
      means_map[mean_int] = 1;
      value = (double)(mean_int - NUM_UNIQUE_MEANS / 2) / 100;
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
      .sampling_rule = BAI_SAMPLING_RULE_TOP_TWO,
      .threshold = BAI_THRESHOLD_GK16,
      .delta = 0.01,
      .sample_limit = 100000,
      .time_limit_seconds = 0,
  };
  ThreadControl *thread_control = thread_control_create();
  BAIResult *bai_result = bai_result_create();
  BAILogger *bai_logger = bai_logger_create("bai_log.txt");

  bai(&bai_options, rvs, rng, thread_control, bai_logger, bai_result);

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
  const char *bai_input_filename = getenv("BAI_INPUT");
  const char *bai_params_index = getenv("BAI_PARAMS_INDEX");
  const char *bai_seed = getenv("BAI_SEED");
  if (bai_input_filename && bai_params_index) {
    test_bai_input_from_file(bai_input_filename, bai_params_index);
  } else if (bai_seed) {
    test_bai_from_seed(bai_seed);
  } else {
    const int num_threads[] = {1, 11};
    const int num_thread_tests = sizeof(num_threads) / sizeof(int);
    for (int i = 0; i < num_thread_tests; i++) {
      const int num_threads_i = num_threads[i];
      test_bai_sample_limit(num_threads_i);
      test_bai_time_limit(num_threads_i);
      test_bai_top_two(num_threads_i);
      test_bai_epigons(num_threads_i);
    }
  }
}
