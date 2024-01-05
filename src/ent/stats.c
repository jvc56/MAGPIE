#include "stats.h"

#include <math.h>
#include <stdlib.h>

#include "../util/util.h"

struct Stat {
  uint64_t cardinality;
  uint64_t weight;
  double mean;
  double sum_of_mean_differences_squared;
};

void stat_reset(Stat *stat) {
  stat->cardinality = 0;
  stat->weight = 0;
  stat->mean = 0;
  stat->sum_of_mean_differences_squared = 0;
}

Stat *stat_create() {
  Stat *stat = malloc_or_die(sizeof(Stat));
  stat_reset(stat);
  return stat;
}

void stat_destroy(Stat *stat) {
  if (!stat) {
    return;
  }
  free(stat);
}

void push_with_cardinality(Stat *stat, double value, uint64_t value_weight,
                           uint64_t cardinality) {
  stat->cardinality += cardinality;
  stat->weight += value_weight;
  double old_mean = stat->mean;
  double value_minus_old_mean = ((double)value) - old_mean;
  stat->mean =
      old_mean + (((double)value_weight) / stat->weight) * value_minus_old_mean;
  stat->sum_of_mean_differences_squared =
      stat->sum_of_mean_differences_squared +
      ((double)value_weight) * value_minus_old_mean *
          (((double)value) - stat->mean);
}

void stat_push(Stat *stat, double value, uint64_t value_weight) {
  push_with_cardinality(stat, value, value_weight, 1);
}

uint64_t stat_get_cardinality(const Stat *stat) { return stat->cardinality; }

uint64_t stat_get_weight(const Stat *stat) { return stat->weight; }

double stat_get_mean(const Stat *stat) { return stat->mean; }

// Use a estimator function to easily change from
// biased to unbiased estimations.
uint64_t get_estimator(uint64_t n) {
  // For now, use a biased estimator for all
  // stats. This might not be ideal since
  // a biased estimator is probably better for
  // inferences which calculate stats for all
  // leaves in the probability space whereas
  // the simulations are a sample.
  // See Bessel's correction for more info.
  return n;
}

double stat_get_variance(const Stat *stat) {
  if (stat->weight <= 1) {
    return 0.0;
  }
  return stat->sum_of_mean_differences_squared /
         (((double)get_estimator(stat->weight)));
}

double stat_get_stdev(const Stat *stat) {
  return sqrt(stat_get_variance(stat));
}

void stats_combine(Stat **stats, int number_of_stats, Stat *combined_stat) {
  uint64_t combined_cardinality = 0;
  uint64_t combined_weight = 0;
  double combined_mean = 0;
  for (int i = 0; i < number_of_stats; i++) {
    uint64_t cardinality = stat_get_cardinality(stats[i]);
    combined_cardinality += cardinality;
    uint64_t weight = stat_get_weight(stats[i]);
    combined_weight += weight;
    combined_mean += stat_get_mean(stats[i]) * weight;
  }
  if (combined_weight <= 0) {
    combined_stat->cardinality = 0;
    combined_stat->weight = 0;
    combined_stat->mean = 0;
    combined_stat->sum_of_mean_differences_squared = 0;
    return;
  }
  combined_mean = combined_mean / combined_weight;

  double combined_error_sum_of_squares = 0;
  for (int i = 0; i < number_of_stats; i++) {
    double stdev = stat_get_stdev(stats[i]);
    uint64_t weight = stat_get_weight(stats[i]);
    combined_error_sum_of_squares += (stdev * stdev) * get_estimator(weight);
  }

  double combined_sum_of_squares = 0;
  for (int i = 0; i < number_of_stats; i++) {
    uint64_t weight = stat_get_weight(stats[i]);
    double mean = stat_get_mean(stats[i]);
    double mean_diff = (mean - combined_mean);
    combined_sum_of_squares += (mean_diff * mean_diff) * weight;
  }
  double combined_sum_of_mean_differences_squared =
      combined_sum_of_squares + combined_error_sum_of_squares;

  combined_stat->cardinality = combined_cardinality;
  combined_stat->weight = combined_weight;
  combined_stat->mean = combined_mean;
  combined_stat->sum_of_mean_differences_squared =
      combined_sum_of_mean_differences_squared;
}

double stat_get_stderr(const Stat *stat, double m) {
  return m * sqrt(stat_get_variance(stat) / (double)stat->cardinality);
}