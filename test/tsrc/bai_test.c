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

void test_bai_epigons(void) {
  const int num_samples = 500;
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
  for (int num_rvs = 2; num_rvs <= 20; num_rvs++) {
    double *means_and_vars =
        (double *)malloc_or_die(num_rvs * 2 * sizeof(double));
    for (int i = 0; i < num_rvs; i++) {
      means_and_vars[i * 2] = 10;
      means_and_vars[i * 2 + 1] = 20;
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
      printf("running %d rvs with strat %d\n", num_rvs, i);
      BAILogger *bai_logger = bai_logger_create("bai.log");
      bai(strategies[i][0], strategies[i][1], strategies[i][2], rvs, 0.99, rng,
          num_samples, 2, bai_logger);
      bai_logger_flush(bai_logger);
      bai_logger_destroy(bai_logger);
      for (int k = 0; k < num_rvs; k++) {
        if (rvs_is_epigon(rvs, k)) {
          num_epigons++;
        }
      }
      printf("num epigons: %d\n", num_epigons);
      assert(num_epigons == num_rvs - 1);
      rvs_destroy(rvs);
      rvs_destroy(rng);
    }
    free(means_and_vars);
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

  const int result =
      bai(strategies[pi][0], strategies[pi][1], strategies[pi][2], rvs, delta,
          rng, num_samples, 0, bai_logger);

  bai_logger_log_int(bai_logger, "result", result + 1);
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
    // FIXME: uncomment
    // test_bai_track_and_stop();
    test_bai_epigons();
    printf("nan < INFINITY: %d\n", NAN < INFINITY);
  }
}
