#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "../src/stats.h"

#include "test_constants.h"
#include "test_util.h"

void test_stats() {
  Stat *stat = create_stat();

  // Test the empty stat
  assert(get_cardinality(stat) == 0);
  assert(get_weight(stat) == 0);
  assert(within_epsilon(get_mean(stat), 0));
  assert(within_epsilon(get_variance(stat), 0));
  assert(within_epsilon(get_stdev(stat), 0));

  // Single element
  push(stat, 1, 1);
  assert(get_cardinality(stat) == 1);
  assert(get_weight(stat) == 1);
  assert(within_epsilon(get_mean(stat), 1));
  assert(within_epsilon(get_variance(stat), 0));
  assert(within_epsilon(get_stdev(stat), 0));

  // Two elements
  push(stat, 3, 1);
  assert(get_cardinality(stat) == 2);
  assert(get_weight(stat) == 2);
  assert(within_epsilon(get_mean(stat), 2));
  assert(within_epsilon(get_variance(stat), 1));
  assert(within_epsilon(get_stdev(stat), 1));

  push(stat, 7, 1);
  assert(get_cardinality(stat) == 3);
  assert(get_weight(stat) == 3);
  assert(within_epsilon(get_mean(stat), 11.0 / 3.0));
  assert(within_epsilon(get_variance(stat), 6.222222222222222));
  assert(within_epsilon(get_stdev(stat), 2.494438257849294));

  // Push a weighted value
  push(stat, 10, 4);
  assert(get_cardinality(stat) == 4);
  assert(get_weight(stat) == 7);
  assert(within_epsilon(get_mean(stat), 51.0 / 7.0));
  assert(within_epsilon(get_variance(stat), 12.48979591836734925));
  assert(within_epsilon(get_stdev(stat), 3.5340905362437093));

  // Reset
  reset_stat(stat);
  assert(get_cardinality(stat) == 0);
  assert(get_weight(stat) == 0);
  assert(within_epsilon(get_mean(stat), 0));
  assert(within_epsilon(get_variance(stat), 0));
  assert(within_epsilon(get_stdev(stat), 0));

  push(stat, 3, 2);
  push(stat, 6, 5);
  push(stat, 17, 3);
  assert(get_cardinality(stat) == 3);
  assert(get_weight(stat) == 10);
  assert(within_epsilon(get_mean(stat), 87.0 / 10));
  assert(within_epsilon(get_variance(stat), 30.81000000000000227373));
  assert(within_epsilon(get_stdev(stat), 5.5506756345511671923));

  destroy_stat(stat);
}