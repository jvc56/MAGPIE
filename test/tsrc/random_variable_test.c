#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>

#include "../../src/impl/random_variable.h"

#include "test_util.h"

void test_random_variable_uniform(void) {
  const uint64_t num_rvs = 20;
  RandomVariablesArgs rv_args = {
      .type = RANDOM_VARIABLES_UNIFORM,
      .num_rvs = num_rvs,
      .seed = 10,
  };

  RandomVariables *rvs1 = rvs_create(&rv_args);
  assert(rvs_get_num_rvs(rvs1) == num_rvs);
  RandomVariables *rvs2 = rvs_create(&rv_args);
  assert(rvs_get_num_rvs(rvs2) == num_rvs);

  for (uint64_t i = 0; i < 100; i++) {
    const uint64_t k = i % num_rvs;
    assert(within_epsilon(rvs_sample(rvs1, k, 0, NULL),
                          rvs_sample(rvs2, k, 0, NULL)));
  }

  rvs_destroy(rvs1);
  rvs_destroy(rvs2);
}

void test_random_variable_uniform_predetermined(void) {
  const uint64_t num_rvs = 2;
  double samples[] = {0.1, 0.2, 0.3, 0.25, 0.43, -0.73, 0.4};
  const uint64_t num_samples = sizeof(samples) / sizeof(double);
  RandomVariablesArgs rv_args = {
      .type = RANDOM_VARIABLES_UNIFORM_PREDETERMINED,
      .num_rvs = num_rvs,
      .num_samples = num_samples,
      .samples = samples,
  };

  RandomVariables *rvs1 = rvs_create(&rv_args);
  assert(rvs_get_num_rvs(rvs1) == num_rvs);
  RandomVariables *rvs2 = rvs_create(&rv_args);
  assert(rvs_get_num_rvs(rvs2) == num_rvs);

  for (uint64_t i = 0; i < num_samples; i++) {
    const uint64_t k = i % num_rvs;
    const double expected_result = samples[i];
    assert(within_epsilon(expected_result, rvs_sample(rvs1, k, 0, NULL)));
    assert(within_epsilon(expected_result, rvs_sample(rvs2, k, 0, NULL)));
  }

  rvs_destroy(rvs1);
  rvs_destroy(rvs2);
}

void test_random_variable_normal(void) {
  double means_and_vars[] = {1, 2, 3, 4, 5, 6, 0.3, 0.0, -2, 60};
  const uint64_t num_rvs = (sizeof(means_and_vars)) / (sizeof(double) * 2);
  RandomVariablesArgs rv_args = {
      .type = RANDOM_VARIABLES_NORMAL,
      .num_rvs = num_rvs,
      .means_and_vars = means_and_vars,
      .seed = 10,
  };

  RandomVariables *rvs1 = rvs_create(&rv_args);
  assert(rvs_get_num_rvs(rvs1) == num_rvs);
  RandomVariables *rvs2 = rvs_create(&rv_args);
  assert(rvs_get_num_rvs(rvs2) == num_rvs);

  for (uint64_t i = 0; i < 100; i++) {
    const uint64_t k = i % num_rvs;
    assert(within_epsilon(rvs_sample(rvs1, k, 0, NULL),
                          rvs_sample(rvs2, k, 0, NULL)));
  }

  rvs_destroy(rvs1);
  rvs_destroy(rvs2);
}

void test_random_variable_normal_predetermined(void) {
  double means_and_vars[] = {1, 2, 3, 4};
  const uint64_t num_rvs = (sizeof(means_and_vars)) / (sizeof(double) * 2);

  double samples[] = {0.1, -0.2, 0.3, -0.25, 0.43, -1.3, 4};
  const uint64_t num_samples = sizeof(samples) / sizeof(double);
  RandomVariablesArgs rv_args = {
      .type = RANDOM_VARIABLES_NORMAL_PREDETERMINED,
      .num_rvs = num_rvs,
      .means_and_vars = means_and_vars,
      .num_samples = num_samples,
      .samples = samples,
  };

  RandomVariables *rvs1 = rvs_create(&rv_args);
  assert(rvs_get_num_rvs(rvs1) == num_rvs);
  RandomVariables *rvs2 = rvs_create(&rv_args);
  assert(rvs_get_num_rvs(rvs2) == num_rvs);

  for (uint64_t i = 0; i < num_samples; i++) {
    const uint64_t k = i % num_rvs;
    const double expected_result =
        means_and_vars[(ptrdiff_t)(k * 2)] +
        sqrt(means_and_vars[(ptrdiff_t)(k * 2 + 1)]) * samples[i];
    assert(within_epsilon(expected_result, rvs_sample(rvs1, k, 0, NULL)));
    assert(within_epsilon(expected_result, rvs_sample(rvs2, k, 0, NULL)));
  }

  rvs_destroy(rvs1);
  rvs_destroy(rvs2);
}

void test_random_variable(void) {
  test_random_variable_uniform();
  test_random_variable_uniform_predetermined();
  test_random_variable_normal();
  test_random_variable_normal_predetermined();
}