
#include "../../src/ent/random_variable.h"

#include "test_util.h"

void test_random_variable_normal(void) {
  double means_and_stdevs[] = {1, 2, 3, 4, 5, 6, 0.3, 0.0, -2, 60};
  const int num_rvs = (sizeof(means_and_stdevs)) / (sizeof(double) * 2);
  RandomVariablesArgs rv_args = {
      .type = RANDOM_VARIABLES_NORMAL,
      .num_rvs = num_rvs,
      .means_and_stdevs = means_and_stdevs,
      .normal.seed = 10,
  };

  RandomVariables *rvs1 = rvs_create(&rv_args);
  assert(rvs_get_num_rvs(rvs1) == num_rvs);
  RandomVariables *rvs2 = rvs_create(&rv_args);
  assert(rvs_get_num_rvs(rvs2) == num_rvs);

  for (int i = 0; i < 100; i++) {
    const int k = i % num_rvs;
    assert(
        within_epsilon(rvs_sample(rvs1, k, NULL), rvs_sample(rvs2, k, NULL)));
  }

  rvs_destroy(rvs1);
  rvs_destroy(rvs2);
}

void test_random_variable_normal_predetermined(void) {
  double means_and_stdevs[] = {1, 2, 3, 4};
  const int num_rvs = (sizeof(means_and_stdevs)) / (sizeof(double) * 2);

  double samples[] = {0.1, -0.2, 0.3, -0.25, 0.43, -1.3, 4};
  const int num_samples = sizeof(samples) / sizeof(double);
  RandomVariablesArgs rv_args = {
      .type = RANDOM_VARIABLES_NORMAL_PREDETERMINED,
      .num_rvs = num_rvs,
      .means_and_stdevs = means_and_stdevs,
      .normal_predetermined.num_samples = num_samples,
      .normal_predetermined.samples = samples,
  };

  RandomVariables *rvs1 = rvs_create(&rv_args);
  assert(rvs_get_num_rvs(rvs1) == num_rvs);
  RandomVariables *rvs2 = rvs_create(&rv_args);
  assert(rvs_get_num_rvs(rvs2) == num_rvs);

  for (int i = 0; i < num_samples; i++) {
    const int k = i % num_rvs;
    const double expected_result =
        means_and_stdevs[k * 2] + means_and_stdevs[k * 2 + 1] * samples[i];
    assert(within_epsilon(expected_result, rvs_sample(rvs1, k, NULL)));
    assert(within_epsilon(expected_result, rvs_sample(rvs2, k, NULL)));
  }

  rvs_destroy(rvs1);
  rvs_destroy(rvs2);
}

void test_random_variable(void) {
  test_random_variable_normal();
  test_random_variable_normal_predetermined();
}