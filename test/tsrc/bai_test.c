#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../src/ent/random_variable.h"

#include "../../src/impl/bai.h"
#include "../../src/impl/bai_sampling_rule.h"
#include "../../src/util/log.h"

void test_bai_track_and_stop(void) {
  // TODO: test normal track and stop
  RandomVariablesArgs rv_args = {};
  RandomVariables *rvs = rvs_create(&rv_args);
  int result = bai(BAI_SAMPLING_RULE_TRACK_AND_STOP, rvs, 0.05);
  assert(result == 0);
  rvs_destroy(rvs);
}

void test_bai_input_from_file(void) {
  const char *bai_input_filename = getenv("BAI_INPUT");
  if (!bai_input_filename) {
    return;
  }
  FILE *file = fopen(bai_input_filename, "r");
  if (!file) {
    log_fatal("Failed to open BAI_INPUT file: %s\n", bai_input_filename);
  }

  double delta;
  int num_rvs;
  double *means_and_stdevs;
  int num_samples;
  double *samples;

  // Read delta
  if (fscanf(file, "%lf", &delta) != 1) {
    log_fatal("Failed to read delta from file: %s\n", bai_input_filename);
  }

  // Read num_rvs
  if (fscanf(file, "%d", &num_rvs) != 1) {
    log_fatal("Failed to read num_rvs from file: %s\n", bai_input_filename);
  }

  // Allocate memory for means_and_stdevs
  means_and_stdevs = (double *)malloc(
      num_rvs * 2 * sizeof(double)); // 2 values per RV (mean and std dev)
  if (!means_and_stdevs) {
    log_fatal("Failed to allocate memory for means_and_stdevs.\n");
  }

  // Read means_and_stdevs
  for (int i = 0; i < num_rvs; ++i) {
    if (fscanf(file, "%lf,%lf", &means_and_stdevs[i * 2],
               &means_and_stdevs[i * 2 + 1]) != 2) {
      log_fatal("Failed to read means and stdevs from file at index %d.\n", i);
    }
  }

  // Read num_samples
  if (fscanf(file, "%d", &num_samples) != 1) {
    log_fatal("Failed to read num_samples from file: %s\n", bai_input_filename);
  }

  // Allocate memory for samples
  samples = (double *)malloc(num_samples * sizeof(double));
  if (!samples) {
    log_fatal("Failed to allocate memory for samples.\n");
  }

  // Read samples
  for (int i = 0; i < num_samples; ++i) {
    if (fscanf(file, "%lf", &samples[i]) != 1) {
      log_fatal("Failed to read sample %d from file.\n", i);
    }
  }

  fclose(file);

  // At this point, the variables are populated
  // You can now process the data as needed

  // Print everything that was read
  printf("delta: %.6f\n", delta);
  printf("num_rvs: %d\n", num_rvs);
  printf("Means and standard deviations (each pair of values corresponds to "
         "mean and stdev for each RV):\n");
  for (int i = 0; i < num_rvs; ++i) {
    printf("RV %d -> Mean: %.6f, Stdev: %.6f\n", i + 1, means_and_stdevs[i * 2],
           means_and_stdevs[i * 2 + 1]);
  }

  printf("num_samples: %d\n", num_samples);
  printf("Samples:\n");
  for (int i = 0; i < num_samples; ++i) {
    printf("Sample %d: %.6f\n", i + 1, samples[i]);
  }

  RandomVariablesArgs rv_args = {
      .type = RANDOM_VARIABLES_NORMAL,
      .num_rvs = num_rvs,
      .means_and_stdevs = means_and_stdevs,
      .rng_args =
          &(RNGArgs){
              .num_samples = num_samples,
              .samples = samples,
              .num_rngs = num_rvs,
          },
  };

  RandomVariables *rvs = rvs_create(&rv_args);
  bai(BAI_SAMPLING_RULE_TRACK_AND_STOP, rvs, delta);

  free(means_and_stdevs);
  free(samples);
  rvs_destroy(rvs);
}

void test_bai(void) { test_bai_input_from_file(); }
