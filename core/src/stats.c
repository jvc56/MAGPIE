#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "constants.h"
#include "stats.h"
#include "util.h"

void reset_stat(Stat *stat) {
  stat->cardinality = 0;
  stat->weight = 0;
  stat->mean = 0;
  stat->sum_of_mean_differences_squared = 0;
}

Stat *create_stat() {
  Stat *stat = malloc_or_die(sizeof(Stat));
  reset_stat(stat);
  return stat;
}

void destroy_stat(Stat *stat) { free(stat); }

Stat *stat_duplicate(const Stat *stat) {
  Stat *new_stat = create_stat();
  new_stat->cardinality = stat->cardinality;
  new_stat->weight = stat->weight;
  new_stat->sum_of_mean_differences_squared =
      stat->sum_of_mean_differences_squared;
  new_stat->mean = stat->mean;
  return new_stat;
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

void push(Stat *stat, double value, uint64_t value_weight) {
  push_with_cardinality(stat, value, value_weight, 1);
}

uint64_t get_cardinality(const Stat *stat) { return stat->cardinality; }

uint64_t get_weight(const Stat *stat) { return stat->weight; }

double get_mean(const Stat *stat) { return stat->mean; }

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

double get_variance(const Stat *stat) {
  if (stat->weight <= 1) {
    return 0.0;
  }
  return stat->sum_of_mean_differences_squared /
         (((double)get_estimator(stat->weight)));
}

double get_stdev(const Stat *stat) { return sqrt(get_variance(stat)); }

void combine_stats(Stat **stats, int number_of_stats, Stat *combined_stat) {
  uint64_t combined_cardinality = 0;
  uint64_t combined_weight = 0;
  double combined_mean = 0;
  for (int i = 0; i < number_of_stats; i++) {
    uint64_t cardinality = get_cardinality(stats[i]);
    combined_cardinality += cardinality;
    uint64_t weight = get_weight(stats[i]);
    combined_weight += weight;
    combined_mean += get_mean(stats[i]) * weight;
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
    double stdev = get_stdev(stats[i]);
    uint64_t weight = get_weight(stats[i]);
    combined_error_sum_of_squares += (stdev * stdev) * get_estimator(weight);
  }

  double combined_sum_of_squares = 0;
  for (int i = 0; i < number_of_stats; i++) {
    uint64_t weight = get_weight(stats[i]);
    double mean = get_mean(stats[i]);
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

double get_standard_error(const Stat *stat, double m) {
  return m * sqrt(get_variance(stat) / (double)stat->cardinality);
}

int round_to_nearest_int(double a) {
  return (int)(a + 0.5 - (a < 0)); // truncated to 55
}