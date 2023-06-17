#ifndef STATS_H
#define STATS_H

#include <stdint.h>

typedef struct Stat {
  uint64_t cardinality;
  uint64_t weight;
  double mean;
  double sum_of_mean_differences_squared;
} Stat;

Stat *create_stat();
void destroy_stat(Stat *stat);
void reset_stat(Stat *stat);
Stat *copy_stat(Stat *original_stat);
void push(Stat *stat, double value, uint64_t weight);
void push_stat(Stat *stat_1, Stat *stat_2);
uint64_t get_cardinality(Stat *stat);
uint64_t get_weight(Stat *stat);
double get_mean(Stat *stat);
double get_variance(Stat *stat);
double get_stdev(Stat *stat);
int round_to_nearest_int(double a);
double get_variance_for_weighted_int_array(uint64_t *weighted_population,
                                           int value_offset);
double get_stdev_for_weighted_int_array(uint64_t *weighted_population,
                                        int value_offset);

#endif