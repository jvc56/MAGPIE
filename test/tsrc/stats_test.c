#include <assert.h>
#include <stdlib.h>

#include "../../src/ent/stats.h"
#include "../../src/util/io_util.h"

#include "test_util.h"

void test_single_stat(void) {
  Stat *stat = stat_create(false);

  // Test the empty stat
  assert(stat_get_num_unique_samples(stat) == 0);
  assert(stat_get_num_samples(stat) == 0);
  assert(within_epsilon(stat_get_mean(stat), 0));
  assert(within_epsilon(stat_get_variance(stat), 0));
  assert(within_epsilon(stat_get_stdev(stat), 0));

  // Single element
  stat_push(stat, 1, 1);
  assert(stat_get_num_unique_samples(stat) == 1);
  assert(stat_get_num_samples(stat) == 1);
  assert(within_epsilon(stat_get_mean(stat), 1));
  assert(within_epsilon(stat_get_variance(stat), 0));
  assert(within_epsilon(stat_get_stdev(stat), 0));

  // Two elements
  stat_push(stat, 3, 1);
  assert(stat_get_num_unique_samples(stat) == 2);
  assert(stat_get_num_samples(stat) == 2);
  assert(within_epsilon(stat_get_mean(stat), 2));
  assert(within_epsilon(stat_get_variance(stat), 1));
  assert(within_epsilon(stat_get_stdev(stat), 1));

  stat_push(stat, 7, 1);
  assert(stat_get_num_unique_samples(stat) == 3);
  assert(stat_get_num_samples(stat) == 3);
  assert(within_epsilon(stat_get_mean(stat), 11.0 / 3.0));
  assert(within_epsilon(stat_get_variance(stat), 6.222222222222222));
  assert(within_epsilon(stat_get_stdev(stat), 2.494438257849294));

  // Push multiple elements of the same value at once
  stat_push(stat, 10, 4);
  assert(stat_get_num_unique_samples(stat) == 4);
  assert(stat_get_num_samples(stat) == 7);
  assert(within_epsilon(stat_get_mean(stat), 51.0 / 7.0));
  assert(within_epsilon(stat_get_variance(stat), 12.48979591836734925));
  assert(within_epsilon(stat_get_stdev(stat), 3.5340905362437093));

  // Reset
  stat_reset(stat);
  assert(stat_get_num_unique_samples(stat) == 0);
  assert(stat_get_num_samples(stat) == 0);
  assert(within_epsilon(stat_get_mean(stat), 0));
  assert(within_epsilon(stat_get_variance(stat), 0));
  assert(within_epsilon(stat_get_stdev(stat), 0));

  stat_push(stat, 3, 2);
  stat_push(stat, 6, 5);
  stat_push(stat, 17, 3);
  assert(stat_get_num_unique_samples(stat) == 3);
  assert(stat_get_num_samples(stat) == 10);
  assert(within_epsilon(stat_get_mean(stat), 87.0 / 10));
  assert(within_epsilon(stat_get_variance(stat), 30.81000000000000227373));
  assert(within_epsilon(stat_get_stdev(stat), 5.5506756345511671923));

  stat_destroy(stat);

  // Create stat with bessel's correction where the mean is estimated
  stat = stat_create(true);

  // Test the empty stat
  assert(stat_get_num_unique_samples(stat) == 0);
  assert(stat_get_num_samples(stat) == 0);
  assert(within_epsilon(stat_get_mean(stat), 0));
  assert(within_epsilon(stat_get_variance(stat), 0));
  assert(within_epsilon(stat_get_stdev(stat), 0));

  // Single element
  stat_push(stat, 1, 1);
  assert(stat_get_num_unique_samples(stat) == 1);
  assert(stat_get_num_samples(stat) == 1);
  assert(within_epsilon(stat_get_mean(stat), 1));
  assert(within_epsilon(stat_get_variance(stat), 0));
  assert(within_epsilon(stat_get_stdev(stat), 0));

  // Two elements
  stat_push(stat, 3, 1);
  assert(stat_get_num_unique_samples(stat) == 2);
  assert(stat_get_num_samples(stat) == 2);
  assert(within_epsilon(stat_get_mean(stat), 2));
  assert(within_epsilon(stat_get_variance(stat), 2));
  assert(within_epsilon(stat_get_stdev(stat), 1.4142135623731));

  stat_push(stat, 7, 1);
  assert(stat_get_num_unique_samples(stat) == 3);
  assert(stat_get_num_samples(stat) == 3);
  assert(within_epsilon(stat_get_mean(stat), 11.0 / 3.0));
  assert(within_epsilon(stat_get_variance(stat), 9.3333333333333));
  assert(within_epsilon(stat_get_stdev(stat), 3.0550504633039));
  assert(within_epsilon(stat_get_sem(stat), 1.7638342073764));
  assert(within_epsilon(stat_get_margin_of_error(stat, 1.645), 2.901507));

  // Push multiple elements of the same value at once
  stat_push(stat, 10, 4);
  assert(stat_get_num_unique_samples(stat) == 4);
  assert(stat_get_num_samples(stat) == 7);
  assert(within_epsilon(stat_get_mean(stat), 51.0 / 7.0));
  assert(within_epsilon(stat_get_variance(stat), 14.571428571429));
  assert(within_epsilon(stat_get_stdev(stat), 3.8172540616821));
  assert(within_epsilon(stat_get_sem(stat), 1.442786419766));
  assert(within_epsilon(stat_get_margin_of_error(stat, 1.645), 2.373384));

  // Reset
  stat_reset(stat);
  assert(stat_get_num_unique_samples(stat) == 0);
  assert(stat_get_num_samples(stat) == 0);
  assert(within_epsilon(stat_get_mean(stat), 0));
  assert(within_epsilon(stat_get_variance(stat), 0));
  assert(within_epsilon(stat_get_stdev(stat), 0));
  assert(within_epsilon(stat_get_sem(stat), 0));
  assert(within_epsilon(stat_get_margin_of_error(stat, 1.645), 0));

  stat_push(stat, 3, 2);
  stat_push(stat, 6, 5);
  stat_push(stat, 17, 3);
  assert(stat_get_num_unique_samples(stat) == 3);
  assert(stat_get_num_samples(stat) == 10);
  assert(within_epsilon(stat_get_mean(stat), 87.0 / 10));
  assert(within_epsilon(stat_get_variance(stat), 34.233333333333));
  assert(within_epsilon(stat_get_stdev(stat), 5.8509258526607));
  assert(within_epsilon(stat_get_sem(stat), 1.8502252115171));
  assert(within_epsilon(stat_get_margin_of_error(stat, 1.645), 3.043620));

  stat_destroy(stat);
}

void test_combined_stats(void) {
  Stat **fragmented_stats = malloc_or_die(sizeof(Stat *) * 10);

  Stat *singular_stat = stat_create(false);
  Stat *combined_stat = stat_create(false);
  fragmented_stats[0] = stat_create(false);
  fragmented_stats[1] = stat_create(false);
  fragmented_stats[2] = stat_create(false);

  stats_combine(fragmented_stats, 3, combined_stat);
  assert(stat_get_num_unique_samples(combined_stat) == 0);
  assert(stat_get_num_samples(combined_stat) == 0);
  assert(stat_get_mean(combined_stat) == 0);
  assert(stat_get_stdev(combined_stat) == 0);

  stat_push(fragmented_stats[0], 7, 1);
  stat_push(singular_stat, 7, 1);

  stats_combine(fragmented_stats, 3, combined_stat);
  assert(within_epsilon(stat_get_stdev(combined_stat),
                        stat_get_stdev(singular_stat)));
  assert(within_epsilon(stat_get_mean(combined_stat),
                        stat_get_mean(singular_stat)));
  assert(stat_get_num_samples(combined_stat) ==
         stat_get_num_samples(singular_stat));
  assert(stat_get_num_unique_samples(combined_stat) ==
         stat_get_num_unique_samples(singular_stat));

  stat_push(fragmented_stats[1], 3, 1);
  stat_push(singular_stat, 3, 1);

  stat_push(fragmented_stats[2], 6, 1);
  stat_push(singular_stat, 6, 1);

  stats_combine(fragmented_stats, 3, combined_stat);
  assert(within_epsilon(stat_get_stdev(combined_stat),
                        stat_get_stdev(singular_stat)));
  assert(within_epsilon(stat_get_mean(combined_stat),
                        stat_get_mean(singular_stat)));
  assert(stat_get_num_samples(combined_stat) ==
         stat_get_num_samples(singular_stat));
  assert(stat_get_num_unique_samples(combined_stat) ==
         stat_get_num_unique_samples(singular_stat));

  stat_push(fragmented_stats[1], 10, 3);
  stat_push(singular_stat, 10, 3);

  stats_combine(fragmented_stats, 3, combined_stat);
  assert(within_epsilon(stat_get_stdev(combined_stat),
                        stat_get_stdev(singular_stat)));
  assert(within_epsilon(stat_get_mean(combined_stat),
                        stat_get_mean(singular_stat)));
  assert(stat_get_num_samples(combined_stat) ==
         stat_get_num_samples(singular_stat));
  assert(stat_get_num_unique_samples(combined_stat) ==
         stat_get_num_unique_samples(singular_stat));
  stat_push(fragmented_stats[2], 4, 6);
  stat_push(singular_stat, 4, 6);

  stats_combine(fragmented_stats, 3, combined_stat);
  assert(within_epsilon(stat_get_stdev(combined_stat),
                        stat_get_stdev(singular_stat)));
  assert(within_epsilon(stat_get_mean(combined_stat),
                        stat_get_mean(singular_stat)));
  assert(stat_get_num_samples(combined_stat) ==
         stat_get_num_samples(singular_stat));
  assert(stat_get_num_unique_samples(combined_stat) ==
         stat_get_num_unique_samples(singular_stat));

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