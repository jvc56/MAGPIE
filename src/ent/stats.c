#include "stats.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>

#include "../util/io_util.h"

struct Stat {
  uint64_t num_unique_samples;
  uint64_t num_samples;
  double mean;
  double sum_of_mean_differences_squared;
  bool mean_is_estimated;
};

void stat_reset(Stat *stat) {
  stat->num_unique_samples = 0;
  stat->num_samples = 0;
  stat->mean = 0;
  stat->sum_of_mean_differences_squared = 0;
}

Stat *stat_create(bool mean_is_estimated) {
  Stat *stat = malloc_or_die(sizeof(Stat));
  stat_reset(stat);
  stat->mean_is_estimated = mean_is_estimated;
  return stat;
}

void stat_destroy(Stat *stat) {
  if (!stat) {
    return;
  }
  free(stat);
}

void stat_push(Stat *stat, double value, uint64_t value_num_samples) {
  stat->num_unique_samples++;
  stat->num_samples += value_num_samples;
  double old_mean = stat->mean;
  double value_minus_old_mean = ((double)value) - old_mean;
  stat->mean = old_mean + (((double)value_num_samples) / stat->num_samples) *
                              value_minus_old_mean;
  stat->sum_of_mean_differences_squared =
      stat->sum_of_mean_differences_squared +
      ((double)value_num_samples) * value_minus_old_mean *
          (((double)value) - stat->mean);
}

uint64_t stat_get_num_unique_samples(const Stat *stat) {
  return stat->num_unique_samples;
}

uint64_t stat_get_num_samples(const Stat *stat) { return stat->num_samples; }

double stat_get_mean(const Stat *stat) { return stat->mean; }

// Applies Bessel's correction if the exact mean is unknown
uint64_t get_estimator(const Stat *stat) {
  if (stat->mean_is_estimated) {
    return stat->num_samples - 1;
  }
  return stat->num_samples;
}

double stat_get_variance(const Stat *stat) {
  if (stat->num_samples <= 1) {
    return 0.0;
  }
  return stat->sum_of_mean_differences_squared / ((double)get_estimator(stat));
}

double stat_get_stdev(const Stat *stat) {
  return sqrt(stat_get_variance(stat));
}

double stat_get_sem(const Stat *stat) {
  if (!stat->mean_is_estimated) {
    log_fatal(
        "standard error of the mean is not defined for non-estimated means\n");
  }
  if (stat->num_samples <= 1) {
    return 0.0;
  }
  return stat_get_stdev(stat) / sqrt((double)stat->num_samples);
}

double stat_get_margin_of_error(const Stat *stat, double zval) {
  return zval * stat_get_sem(stat);
}

double stats_get_welch_t(const Stat *stat1, const Stat *stat2) {
  double mean1 = stat_get_mean(stat1);
  double mean2 = stat_get_mean(stat2);
  double sem1 = stat_get_sem(stat1);
  double sem2 = stat_get_sem(stat2);
  return (mean1 - mean2) / sqrt(sem1 * sem1 + sem2 * sem2);
}

// Returns true if stat1 is greater than stat2
// with a statistical significance of zval.
bool stats_is_greater_than(const Stat *stat1, const Stat *stat2, double zval) {
  return stats_get_welch_t(stat1, stat2) > zval;
}

void stats_combine(Stat **stats, int number_of_stats, Stat *combined_stat) {
  uint64_t combined_num_unique_samples = 0;
  uint64_t combined_num_samples = 0;
  double combined_mean = 0;
  for (int i = 0; i < number_of_stats; i++) {
    uint64_t num_unique_samples = stat_get_num_unique_samples(stats[i]);
    combined_num_unique_samples += num_unique_samples;
    uint64_t num_samples = stat_get_num_samples(stats[i]);
    combined_num_samples += num_samples;
    combined_mean += stat_get_mean(stats[i]) * num_samples;
  }
  if (combined_num_samples <= 0) {
    combined_stat->num_unique_samples = 0;
    combined_stat->num_samples = 0;
    combined_stat->mean = 0;
    combined_stat->sum_of_mean_differences_squared = 0;
    return;
  }
  combined_mean = combined_mean / combined_num_samples;

  double combined_error_sum_of_squares = 0;
  for (int i = 0; i < number_of_stats; i++) {
    double stdev = stat_get_stdev(stats[i]);
    combined_error_sum_of_squares += (stdev * stdev) * get_estimator(stats[i]);
  }

  double combined_sum_of_squares = 0;
  for (int i = 0; i < number_of_stats; i++) {
    uint64_t num_samples = stat_get_num_samples(stats[i]);
    double mean = stat_get_mean(stats[i]);
    double mean_diff = (mean - combined_mean);
    combined_sum_of_squares += (mean_diff * mean_diff) * num_samples;
  }
  double combined_sum_of_mean_differences_squared =
      combined_sum_of_squares + combined_error_sum_of_squares;

  combined_stat->num_unique_samples = combined_num_unique_samples;
  combined_stat->num_samples = combined_num_samples;
  combined_stat->mean = combined_mean;
  combined_stat->sum_of_mean_differences_squared =
      combined_sum_of_mean_differences_squared;
}
