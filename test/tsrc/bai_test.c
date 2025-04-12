#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../src/ent/bai_logger.h"
#include "../../src/ent/random_variable.h"

#include "../../src/impl/bai.h"
#include "../../src/impl/bai_sampling_rule.h"
#include "../../src/util/log.h"

static const int strategies[][3] = {
    {BAI_SAMPLING_RULE_TRACK_AND_STOP, false, BAI_THRESHOLD_GK16},
    {BAI_SAMPLING_RULE_TRACK_AND_STOP, true, BAI_THRESHOLD_GK16},
    {BAI_SAMPLING_RULE_TOP_TWO, false, BAI_THRESHOLD_GK16},
    {BAI_SAMPLING_RULE_TOP_TWO, true, BAI_THRESHOLD_GK16},
    {BAI_SAMPLING_RULE_ROUND_ROBIN, false, BAI_THRESHOLD_GK16},
    {BAI_SAMPLING_RULE_TRACK_AND_STOP, false, BAI_THRESHOLD_HT},
    {BAI_SAMPLING_RULE_TRACK_AND_STOP, true, BAI_THRESHOLD_HT},
    {BAI_SAMPLING_RULE_TOP_TWO, false, BAI_THRESHOLD_HT},
    {BAI_SAMPLING_RULE_TOP_TWO, true, BAI_THRESHOLD_HT},
    {BAI_SAMPLING_RULE_ROUND_ROBIN, false, BAI_THRESHOLD_HT},
};
static const int num_strategies_entries =
    sizeof(strategies) / sizeof(strategies[0]);

void test_bai_track_and_stop(void) {
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
  int result = bai(BAI_SAMPLING_RULE_TRACK_AND_STOP, true, BAI_THRESHOLD_HT,
                   rvs, 0.05, rng, 1000000000, 0, NULL);
  assert(result == 1);
  rvs_destroy(rng);
  rvs_destroy(rvs);
}

// Assumes rv_args are normal predetermined
// Assumes rng_args are uniform
void write_bai_input(const double delta, const RandomVariablesArgs *rv_args,
                     const RandomVariablesArgs *rng_args) {
  FILE *file = fopen("normal_data.txt", "w");
  fprintf(file, "%0.20f\n", delta);
  fprintf(file, "%lu\n", rv_args->num_rvs);
  for (uint64_t i = 0; i < rv_args->num_rvs; i++) {
    fprintf(file, "%0.20f,%0.20f\n", rv_args->means_and_vars[i * 2],
            rv_args->means_and_vars[i * 2 + 1]);
  }
  fprintf(file, "%lu\n", rv_args->num_samples);
  for (uint64_t i = 0; i < rv_args->num_samples; i++) {
    fprintf(file, "%0.20f\n", rv_args->samples[i]);
  }
  RandomVariables *rng = rvs_create(rng_args);
  for (uint64_t i = 0; i < rv_args->num_samples; i++) {
    fprintf(file, "%0.20f\n", rvs_sample(rng, 0, NULL));
  }
  rvs_destroy(rng);
  fclose(file);
}

void test_bai_epigons(void) {
  const int num_samples = 5000;
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
  const double delta = 0.01;
  for (int max_classes = 1; max_classes <= 3; max_classes++) {
    for (int num_rvs = 2; num_rvs <= 10; num_rvs++) {
      double *means_and_vars =
          (double *)malloc_or_die(num_rvs * 2 * sizeof(double));
      int expected_epigons = 0;
      for (int i = 0; i < num_rvs; i++) {
        means_and_vars[i * 2] = 3 * (max_classes - (i % max_classes));
        means_and_vars[i * 2 + 1] = 5 * (max_classes - (i % max_classes));
        if (i > 0 && i % max_classes == 0) {
          expected_epigons++;
        }
      }
      rv_args.num_rvs = num_rvs;
      rv_args.means_and_vars = means_and_vars;
      rng_args.num_rvs = num_rvs;
      for (int i = 0; i < num_strategies_entries; i++) {
        RandomVariables *rvs = rvs_create(&rv_args);
        RandomVariables *rng = rvs_create(&rng_args);

        int num_epigons = 0;
        for (int k = 0; k < num_rvs; k++) {
          if (rvs_is_epigon(rvs, k)) {
            num_epigons++;
          }
        }
        assert(num_epigons == 0);
        // Use something like
        // BAILogger *bai_logger = bai_logger_create("bai.log");
        // to log the BAI output for debugging. Logging will significantly
        // increase the runtime.
        BAILogger *bai_logger = NULL;
        const int result =
            bai(strategies[i][0], strategies[i][1], strategies[i][2], rvs,
                delta, rng, num_samples, 100, bai_logger);
        bai_logger_log_int(bai_logger, "result", result);
        bai_logger_flush(bai_logger);
        bai_logger_destroy(bai_logger);
        for (int k = 0; k < num_rvs; k++) {
          if (rvs_is_epigon(rvs, k)) {
            num_epigons++;
          }
        }
        const bool is_ok =
            (num_epigons == expected_epigons) && (result % max_classes == 0);
        if (!is_ok) {
          printf("Failed for %d rvs with strat %d and %d classes\nRan the "
                 "following assertions:\n%d == %d (epigons)\n%d %% %d == 0 "
                 "(nonneg result)\n",
                 num_rvs, i, max_classes, num_epigons, expected_epigons, result,
                 max_classes);
          write_bai_input(delta, &rv_args, &rng_args);
          exit(1);
        }
        rvs_destroy(rvs);
        rvs_destroy(rng);
      }
      free(means_and_vars);
    }
  }
  free(samples);
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

  fclose(file);

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
  int result = bai(strategies[pi][0], strategies[pi][1], strategies[pi][2], rvs,
                   delta, rng, num_samples, 0, bai_logger);

  if (result >= 0) {
    result++;
  }

  bai_logger_log_int(bai_logger, "result", result);
  bai_logger_flush(bai_logger);

  bai_logger_destroy(bai_logger);
  rvs_destroy(rvs);
  rvs_destroy(rng);
  free(means_and_vars);
  free(samples);
  free(rng_samples);
}

void test_bai(void) {
  const char *bai_input_filename = getenv("BAI_INPUT");
  const char *bai_params_index = getenv("BAI_PARAMS_INDEX");
  if (bai_input_filename && bai_params_index) {
    test_bai_input_from_file(bai_input_filename, bai_params_index);
  } else {
    test_bai_track_and_stop();
    test_bai_epigons();
  }
}
