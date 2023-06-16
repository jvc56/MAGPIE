#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "constants.h"
#include "stats.h"

void reset_stat(Stat *stat) {
  stat->cardinality = 0;
  stat->weight = 0;
  stat->mean = 0;
  stat->sum_of_mean_differences_squared = 0;
}

Stat *create_stat() {
  Stat *stat = malloc(sizeof(Stat));
  reset_stat(stat);
  return stat;
}

void destroy_stat(Stat *stat) { free(stat); }

Stat *copy_stat(Stat *original_stat) {
  Stat *copy_stat = create_stat();
  copy_stat->cardinality = original_stat->cardinality;
  copy_stat->weight = original_stat->weight;
  copy_stat->sum_of_mean_differences_squared =
      original_stat->sum_of_mean_differences_squared;
  copy_stat->mean = original_stat->mean;
  return copy_stat;
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

uint64_t get_cardinality(Stat *stat) { return stat->cardinality; }

uint64_t get_weight(Stat *stat) { return stat->weight; }

double get_mean(Stat *stat) { return stat->mean; }

void push_stat(Stat *stat_1, Stat *stat_2) {
  push_with_cardinality(stat_1, get_mean(stat_2), get_weight(stat_2),
                        get_cardinality(stat_2));
}

double get_variance(Stat *stat) {
  if (stat->weight <= 1) {
    return 0.0;
  }
  // Use population variance since the stat
  // has data for the entire probability space.
  return stat->sum_of_mean_differences_squared / (((double)stat->weight));
}

double get_stdev(Stat *stat) { return sqrt(get_variance(stat)); }

int round_to_nearest_int(double a) {
  return (int)(a + 0.5 - (a < 0)); // truncated to 55
}

double get_variance_for_weighted_int_array(int *weighted_population,
                                           int value_offset) {
  uint64_t sum_of_weights = 0;
  int64_t sum_of_values = 0;
  uint64_t sum_of_values_squared = 0;
  for (int value = 0; value < (NUMBER_OF_ROUNDED_EQUITY_VALUES); value++) {
    int weight = weighted_population[value];
    sum_of_weights += weight;
    int adjusted_value = value + value_offset;
    sum_of_values += weight * adjusted_value;
    sum_of_values_squared += adjusted_value * adjusted_value * weight;
  }
  if (sum_of_weights == 0) {
    return 0;
  }
  double mean = (double)sum_of_values / (double)sum_of_weights;
  return ((double)sum_of_values_squared / (double)sum_of_weights) -
         (mean * mean);
}

double get_stdev_for_weighted_int_array(int *weighted_population,
                                        int value_offset) {
  double variance =
      get_variance_for_weighted_int_array(weighted_population, value_offset);
  if (variance == 0) {
    return 0;
  }
  return sqrt(variance);
}