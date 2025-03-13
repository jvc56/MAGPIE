
#include "../../src/ent/random_variable.h"

#include "test_util.h"

void test_random_variable_normal(void) {
  int num_rvs = 4;
  double *means_and_stdevs = malloc_or_die(num_rvs * sizeof(double));

  RandomVariablesArgs rv_args = {
      .type = RANDOM_VARIABLES_NORMAL,
      .num_rvs = num_rvs,
      .normal.means_and_stdevs = malloc_or_die(sizeof(double) * 2),
      .normal.seed = 10,
  };

  RandomVariables *rvs = rvs_create(&rv_args);
  assert(rvs_get_num_rvs(rvs) == num_rvs);

  // Check that random variables are using the same rng
  assert(within_epsilon(rvs_sample(rvs, 0), rvs_sample(rvs, 1)));
  assert(within_epsilon(rvs_sample(rvs, 2), rvs_sample(rvs, 3)));

  assert(within_epsilon(rvs_sample(rvs, 0), rvs_sample(rvs, 2)));
  assert(within_epsilon(rvs_sample(rvs, 1), rvs_sample(rvs, 3)));

  assert(within_epsilon(rvs_sample(rvs, 0), rvs_sample(rvs, 3)));
  assert(within_epsilon(rvs_sample(rvs, 1), rvs_sample(rvs, 2)));

  rvs_destroy(rvs);
}

void test_random_variable(void) {
  test_random_variable_normal();
  test_random_variable_precomputed();
}