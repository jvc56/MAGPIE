#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../src/ent/bai_logger.h"
#include "../../src/ent/random_variable.h"

#include "../../src/impl/bai.h"
#include "../../src/impl/bai_sampling_rule.h"
#include "../../src/util/log.h"

void test_bai_track_and_stop(void) {
  // TODO: test normal track and stop
  double means_and_stdevs[] = {0, 1, 100, 1};
  RandomVariablesArgs rv_args = {
      .type = RANDOM_VARIABLES_NORMAL,
      .num_rvs = (sizeof(means_and_stdevs) / sizeof(double)) / 2,
      .means_and_stdevs = means_and_stdevs,
      .normal.seed = 10,
  };
  RandomVariables *rvs = rvs_create(&rv_args);
  int result = bai(BAI_SAMPLING_RULE_TRACK_AND_STOP, rvs, 0.05, NULL);
  assert(result == 1);
  rvs_destroy(rvs);
}

void test_bai_input_from_file(const char *bai_input_filename) {
  FILE *file = fopen(bai_input_filename, "r");
  if (!file) {
    log_fatal("Failed to open BAI_INPUT file: %s\n", bai_input_filename);
  }

  double delta;
  int num_rvs;
  double *means_and_stdevs;
  int num_samples;
  double *samples;

  if (fscanf(file, "%lf", &delta) != 1) {
    log_fatal("Failed to read delta from file: %s\n", bai_input_filename);
  }

  if (fscanf(file, "%d", &num_rvs) != 1) {
    log_fatal("Failed to read num_rvs from file: %s\n", bai_input_filename);
  }

  means_and_stdevs = (double *)malloc(
      num_rvs * 2 * sizeof(double)); // 2 values per RV (mean and std dev)
  if (!means_and_stdevs) {
    log_fatal("Failed to allocate memory for means_and_stdevs.\n");
  }

  for (int i = 0; i < num_rvs; ++i) {
    if (fscanf(file, "%lf,%lf", &means_and_stdevs[i * 2],
               &means_and_stdevs[i * 2 + 1]) != 2) {
      log_fatal("Failed to read means and stdevs from file at index %d.\n", i);
    }
  }

  if (fscanf(file, "%d", &num_samples) != 1) {
    log_fatal("Failed to read num_samples from file: %s\n", bai_input_filename);
  }

  samples = (double *)malloc(num_samples * sizeof(double));
  if (!samples) {
    log_fatal("Failed to allocate memory for samples.\n");
  }

  for (int i = 0; i < num_samples; ++i) {
    if (fscanf(file, "%lf", &samples[i]) != 1) {
      log_fatal("Failed to read sample %d from file.\n", i);
    }
  }

  fclose(file);

  BAILogger *bai_logger = bai_logger_create("bai_log_magpie.txt");

  bai_logger_log_double(bai_logger, "delta", delta);
  bai_logger_log_int(bai_logger, "num_rvs", num_rvs);
  bai_logger_log_double_array(bai_logger, "means_and_stddevs", means_and_stdevs,
                              num_rvs * 2);
  bai_logger_log_int(bai_logger, "num_samples", num_samples);
  bai_logger_flush(bai_logger);

  RandomVariablesArgs rv_args = {
      .type = RANDOM_VARIABLES_NORMAL_PREDETERMINED,
      .num_rvs = num_rvs,
      .means_and_stdevs = means_and_stdevs,
      .normal_predetermined.num_samples = num_samples,
      .normal_predetermined.samples = samples,
  };

  RandomVariables *rvs = rvs_create(&rv_args);
  const int result =
      bai(BAI_SAMPLING_RULE_TRACK_AND_STOP, rvs, delta, bai_logger);
  bai_logger_log_int(bai_logger, "result", result + 1);
  bai_logger_flush(bai_logger);

  bai_logger_destroy(bai_logger);
  rvs_destroy(rvs);
  free(means_and_stdevs);
  free(samples);
}

void test_bai(void) {
  const char *bai_input_filename = getenv("BAI_INPUT");
  if (bai_input_filename) {
    test_bai_input_from_file(bai_input_filename);
  } else {
    test_bai_track_and_stop();
  }
}
