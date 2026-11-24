#include "stats.h"

#include "../util/io_util.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct Stat {
  uint64_t num_unique_samples;
  uint64_t num_samples;
  double total_weight;
  double mean;
  double sum_of_mean_differences_squared;
  bool mean_is_estimated;
  bool has_weights;
};

void stat_reset(Stat *stat) {
  stat->num_unique_samples = 0;
  stat->num_samples = 0;
  stat->total_weight = 0;
  stat->mean = 0;
  stat->sum_of_mean_differences_squared = 0;
  stat->has_weights = false;
}

Stat *stat_create(bool mean_is_estimated) {
  Stat *stat = malloc_or_die(sizeof(Stat));
  stat_reset(stat);
  stat->mean_is_estimated = mean_is_estimated;
  return stat;
}

void stat_copy(Stat *dst, const Stat *src) { memcpy(dst, src, sizeof(Stat)); }

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
  double value_minus_old_mean = value - old_mean;
  stat->mean =
      old_mean + (((double)value_num_samples) / (double)stat->num_samples) *
                     value_minus_old_mean;
  stat->sum_of_mean_differences_squared =
      stat->sum_of_mean_differences_squared +
      ((double)value_num_samples) * value_minus_old_mean * (value - stat->mean);
}

void stat_push_weighted(Stat *stat, double value, uint64_t value_num_samples,
                        double weight) {
  if (!stat->has_weights && stat->num_samples > 0) {
    stat->total_weight = (double)stat->num_samples;
  }
  stat->has_weights = true;
  stat->num_unique_samples++;
  stat->num_samples += value_num_samples;
  stat->total_weight += weight;
  if (stat->total_weight <= 0.0) {
    return;
  }
  const double old_mean = stat->mean;
  const double value_minus_old_mean = value - old_mean;
  stat->mean = old_mean + (weight / stat->total_weight) * value_minus_old_mean;
  stat->sum_of_mean_differences_squared =
      stat->sum_of_mean_differences_squared +
      weight * value_minus_old_mean * (value - stat->mean);
}

uint64_t stat_get_num_unique_samples(const Stat *stat) {
  return stat->num_unique_samples;
}

uint64_t stat_get_num_samples(const Stat *stat) { return stat->num_samples; }

double stat_get_total_weight(const Stat *stat) {
  return stat->has_weights ? stat->total_weight : (double)stat->num_samples;
}

double stat_get_mean(const Stat *stat) { return stat->mean; }

// Applies Bessel's correction if the exact mean is unknown
uint64_t get_estimator(const Stat *stat) {
  if (stat->mean_is_estimated) {
    return stat->num_samples - 1;
  }
  return stat->num_samples;
}

static double get_weight_estimator(const Stat *stat) {
  const double weight = stat_get_total_weight(stat);
  if (stat->mean_is_estimated) {
    return weight - 1.0;
  }
  return weight;
}

double stat_get_variance(const Stat *stat) {
  if (stat->has_weights) {
    if (stat->num_samples <= 1) {
      return 0.0;
    }
    const double estimator = get_weight_estimator(stat);
    if (estimator <= 0.0) {
      return 0.0;
    }
    return stat->sum_of_mean_differences_squared / estimator;
  }
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

void stats_combine(Stat **stats, int number_of_stats, Stat *combined_stat) {
  bool any_weighted = false;
  for (int i = 0; i < number_of_stats; i++) {
    if (stats[i]->has_weights) {
      any_weighted = true;
      break;
    }
  }
  if (any_weighted) {
    uint64_t combined_num_unique_samples = 0;
    uint64_t combined_num_samples = 0;
    double combined_weight = 0.0;
    double combined_mean = 0.0;
    for (int i = 0; i < number_of_stats; i++) {
      combined_num_unique_samples += stat_get_num_unique_samples(stats[i]);
      combined_num_samples += stat_get_num_samples(stats[i]);
      const double weight = stat_get_total_weight(stats[i]);
      combined_weight += weight;
      combined_mean += stat_get_mean(stats[i]) * weight;
    }
    combined_stat->has_weights = true;
    if (combined_num_samples == 0 || combined_weight <= 0.0) {
      combined_stat->num_unique_samples = combined_num_unique_samples;
      combined_stat->num_samples = combined_num_samples;
      combined_stat->total_weight = 0.0;
      combined_stat->mean = 0.0;
      combined_stat->sum_of_mean_differences_squared = 0.0;
      return;
    }
    combined_mean /= combined_weight;

    double combined_error_sum_of_squares = 0.0;
    for (int i = 0; i < number_of_stats; i++) {
      const double stdev = stat_get_stdev(stats[i]);
      combined_error_sum_of_squares +=
          (stdev * stdev) * get_weight_estimator(stats[i]);
    }

    double combined_sum_of_squares = 0.0;
    for (int i = 0; i < number_of_stats; i++) {
      const double weight = stat_get_total_weight(stats[i]);
      const double mean_diff = stat_get_mean(stats[i]) - combined_mean;
      combined_sum_of_squares += (mean_diff * mean_diff) * weight;
    }

    combined_stat->num_unique_samples = combined_num_unique_samples;
    combined_stat->num_samples = combined_num_samples;
    combined_stat->total_weight = combined_weight;
    combined_stat->mean = combined_mean;
    combined_stat->sum_of_mean_differences_squared =
        combined_sum_of_squares + combined_error_sum_of_squares;
    return;
  }
  combined_stat->has_weights = false;
  combined_stat->total_weight = 0.0;
  uint64_t combined_num_unique_samples = 0;
  uint64_t combined_num_samples = 0;
  double combined_mean = 0;
  for (int i = 0; i < number_of_stats; i++) {
    uint64_t num_unique_samples = stat_get_num_unique_samples(stats[i]);
    combined_num_unique_samples += num_unique_samples;
    uint64_t num_samples = stat_get_num_samples(stats[i]);
    combined_num_samples += num_samples;
    combined_mean += stat_get_mean(stats[i]) * (double)num_samples;
  }
  if (combined_num_samples == 0) {
    combined_stat->num_unique_samples = 0;
    combined_stat->num_samples = 0;
    combined_stat->mean = 0;
    combined_stat->sum_of_mean_differences_squared = 0;
    return;
  }
  combined_mean = combined_mean / (double)combined_num_samples;

  double combined_error_sum_of_squares = 0;
  for (int i = 0; i < number_of_stats; i++) {
    double stdev = stat_get_stdev(stats[i]);
    combined_error_sum_of_squares +=
        (stdev * stdev) * (double)get_estimator(stats[i]);
  }

  double combined_sum_of_squares = 0;
  for (int i = 0; i < number_of_stats; i++) {
    uint64_t num_samples = stat_get_num_samples(stats[i]);
    double mean = stat_get_mean(stats[i]);
    double mean_diff = (mean - combined_mean);
    combined_sum_of_squares += (mean_diff * mean_diff) * (double)num_samples;
  }
  double combined_sum_of_mean_differences_squared =
      combined_sum_of_squares + combined_error_sum_of_squares;

  combined_stat->num_unique_samples = combined_num_unique_samples;
  combined_stat->num_samples = combined_num_samples;
  combined_stat->mean = combined_mean;
  combined_stat->sum_of_mean_differences_squared =
      combined_sum_of_mean_differences_squared;
}
