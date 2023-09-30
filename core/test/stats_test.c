#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "../src/stats.h"
#include "../src/util.h"

#include "test_constants.h"
#include "test_util.h"

void test_single_stat() {
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
  assert(within_epsilon(get_standard_error(stat, 2.5), 3.600411499115477));

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

void test_combined_stats() {
  Stat **fragmented_stats = malloc_or_die(sizeof(Stat *) * 10);

  Stat *singular_stat = create_stat();
  Stat *combined_stat = create_stat();
  fragmented_stats[0] = create_stat();
  fragmented_stats[1] = create_stat();
  fragmented_stats[2] = create_stat();

  combine_stats(fragmented_stats, 3, combined_stat);
  assert(get_cardinality(combined_stat) == 0);
  assert(get_weight(combined_stat) == 0);
  assert(get_mean(combined_stat) == 0);
  assert(get_stdev(combined_stat) == 0);

  push(fragmented_stats[0], 7, 1);
  push(singular_stat, 7, 1);

  combine_stats(fragmented_stats, 3, combined_stat);
  assert(within_epsilon(get_stdev(combined_stat), get_stdev(singular_stat)));
  assert(within_epsilon(get_mean(combined_stat), get_mean(singular_stat)));
  assert(get_weight(combined_stat) == get_weight(singular_stat));
  assert(get_cardinality(combined_stat) == get_cardinality(singular_stat));

  push(fragmented_stats[1], 3, 1);
  push(singular_stat, 3, 1);

  push(fragmented_stats[2], 6, 1);
  push(singular_stat, 6, 1);

  combine_stats(fragmented_stats, 3, combined_stat);
  assert(within_epsilon(get_stdev(combined_stat), get_stdev(singular_stat)));
  assert(within_epsilon(get_mean(combined_stat), get_mean(singular_stat)));
  assert(get_weight(combined_stat) == get_weight(singular_stat));
  assert(get_cardinality(combined_stat) == get_cardinality(singular_stat));

  push(fragmented_stats[1], 10, 3);
  push(singular_stat, 10, 3);

  combine_stats(fragmented_stats, 3, combined_stat);
  assert(within_epsilon(get_stdev(combined_stat), get_stdev(singular_stat)));
  assert(within_epsilon(get_mean(combined_stat), get_mean(singular_stat)));
  assert(get_weight(combined_stat) == get_weight(singular_stat));
  assert(get_cardinality(combined_stat) == get_cardinality(singular_stat));
  push(fragmented_stats[2], 4, 6);
  push(singular_stat, 4, 6);

  combine_stats(fragmented_stats, 3, combined_stat);
  assert(within_epsilon(get_stdev(combined_stat), get_stdev(singular_stat)));
  assert(within_epsilon(get_mean(combined_stat), get_mean(singular_stat)));
  assert(get_weight(combined_stat) == get_weight(singular_stat));
  assert(get_cardinality(combined_stat) == get_cardinality(singular_stat));

  for (int i = 0; i < 3; i++) {
    destroy_stat(fragmented_stats[i]);
  }
  destroy_stat(singular_stat);
  destroy_stat(combined_stat);
  free(fragmented_stats);
}

void test_stats() {
  test_single_stat();
  test_combined_stats();
}