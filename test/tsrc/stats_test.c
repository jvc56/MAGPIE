#include <assert.h>
#include <stdlib.h>

#include "../../src/ent/stats.h"
#include "../../src/util/util.h"

#include "test_util.h"

void test_single_stat(void) {
  Stat *stat = stat_create();

  // Test the empty stat
  assert(stat_get_cardinality(stat) == 0);
  assert(stat_get_weight(stat) == 0);
  assert(within_epsilon(stat_get_mean(stat), 0));
  assert(within_epsilon(stat_get_variance(stat), 0));
  assert(within_epsilon(stat_get_stdev(stat), 0));

  // Single element
  stat_push(stat, 1, 1);
  assert(stat_get_cardinality(stat) == 1);
  assert(stat_get_weight(stat) == 1);
  assert(within_epsilon(stat_get_mean(stat), 1));
  assert(within_epsilon(stat_get_variance(stat), 0));
  assert(within_epsilon(stat_get_stdev(stat), 0));

  // Two elements
  stat_push(stat, 3, 1);
  assert(stat_get_cardinality(stat) == 2);
  assert(stat_get_weight(stat) == 2);
  assert(within_epsilon(stat_get_mean(stat), 2));
  assert(within_epsilon(stat_get_variance(stat), 1));
  assert(within_epsilon(stat_get_stdev(stat), 1));

  stat_push(stat, 7, 1);
  assert(stat_get_cardinality(stat) == 3);
  assert(stat_get_weight(stat) == 3);
  assert(within_epsilon(stat_get_mean(stat), 11.0 / 3.0));
  assert(within_epsilon(stat_get_variance(stat), 6.222222222222222));
  assert(within_epsilon(stat_get_stdev(stat), 2.494438257849294));
  assert(within_epsilon(stat_get_stderr(stat, 2.5), 3.600411499115477));

  // Push a weighted value
  stat_push(stat, 10, 4);
  assert(stat_get_cardinality(stat) == 4);
  assert(stat_get_weight(stat) == 7);
  assert(within_epsilon(stat_get_mean(stat), 51.0 / 7.0));
  assert(within_epsilon(stat_get_variance(stat), 12.48979591836734925));
  assert(within_epsilon(stat_get_stdev(stat), 3.5340905362437093));

  // Reset
  stat_reset(stat);
  assert(stat_get_cardinality(stat) == 0);
  assert(stat_get_weight(stat) == 0);
  assert(within_epsilon(stat_get_mean(stat), 0));
  assert(within_epsilon(stat_get_variance(stat), 0));
  assert(within_epsilon(stat_get_stdev(stat), 0));

  stat_push(stat, 3, 2);
  stat_push(stat, 6, 5);
  stat_push(stat, 17, 3);
  assert(stat_get_cardinality(stat) == 3);
  assert(stat_get_weight(stat) == 10);
  assert(within_epsilon(stat_get_mean(stat), 87.0 / 10));
  assert(within_epsilon(stat_get_variance(stat), 30.81000000000000227373));
  assert(within_epsilon(stat_get_stdev(stat), 5.5506756345511671923));

  stat_destroy(stat);
}

void test_combined_stats(void) {
  Stat **fragmented_stats = malloc_or_die(sizeof(Stat *) * 10);

  Stat *singular_stat = stat_create();
  Stat *combined_stat = stat_create();
  fragmented_stats[0] = stat_create();
  fragmented_stats[1] = stat_create();
  fragmented_stats[2] = stat_create();

  stats_combine(fragmented_stats, 3, combined_stat);
  assert(stat_get_cardinality(combined_stat) == 0);
  assert(stat_get_weight(combined_stat) == 0);
  assert(stat_get_mean(combined_stat) == 0);
  assert(stat_get_stdev(combined_stat) == 0);

  stat_push(fragmented_stats[0], 7, 1);
  stat_push(singular_stat, 7, 1);

  stats_combine(fragmented_stats, 3, combined_stat);
  assert(within_epsilon(stat_get_stdev(combined_stat),
                        stat_get_stdev(singular_stat)));
  assert(within_epsilon(stat_get_mean(combined_stat),
                        stat_get_mean(singular_stat)));
  assert(stat_get_weight(combined_stat) == stat_get_weight(singular_stat));
  assert(stat_get_cardinality(combined_stat) ==
         stat_get_cardinality(singular_stat));

  stat_push(fragmented_stats[1], 3, 1);
  stat_push(singular_stat, 3, 1);

  stat_push(fragmented_stats[2], 6, 1);
  stat_push(singular_stat, 6, 1);

  stats_combine(fragmented_stats, 3, combined_stat);
  assert(within_epsilon(stat_get_stdev(combined_stat),
                        stat_get_stdev(singular_stat)));
  assert(within_epsilon(stat_get_mean(combined_stat),
                        stat_get_mean(singular_stat)));
  assert(stat_get_weight(combined_stat) == stat_get_weight(singular_stat));
  assert(stat_get_cardinality(combined_stat) ==
         stat_get_cardinality(singular_stat));

  stat_push(fragmented_stats[1], 10, 3);
  stat_push(singular_stat, 10, 3);

  stats_combine(fragmented_stats, 3, combined_stat);
  assert(within_epsilon(stat_get_stdev(combined_stat),
                        stat_get_stdev(singular_stat)));
  assert(within_epsilon(stat_get_mean(combined_stat),
                        stat_get_mean(singular_stat)));
  assert(stat_get_weight(combined_stat) == stat_get_weight(singular_stat));
  assert(stat_get_cardinality(combined_stat) ==
         stat_get_cardinality(singular_stat));
  stat_push(fragmented_stats[2], 4, 6);
  stat_push(singular_stat, 4, 6);

  stats_combine(fragmented_stats, 3, combined_stat);
  assert(within_epsilon(stat_get_stdev(combined_stat),
                        stat_get_stdev(singular_stat)));
  assert(within_epsilon(stat_get_mean(combined_stat),
                        stat_get_mean(singular_stat)));
  assert(stat_get_weight(combined_stat) == stat_get_weight(singular_stat));
  assert(stat_get_cardinality(combined_stat) ==
         stat_get_cardinality(singular_stat));

  for (int i = 0; i < 3; i++) {
    stat_destroy(fragmented_stats[i]);
  }
  stat_destroy(singular_stat);
  stat_destroy(combined_stat);
  free(fragmented_stats);
}

void test_stats(void) {
  test_single_stat();
  test_combined_stats();
}