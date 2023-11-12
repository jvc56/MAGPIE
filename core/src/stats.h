#ifndef STATS_H
#define STATS_H

#include <stdint.h>

// Z-distribution confidence values
#define STATS_Z95 1.96
#define STATS_Z98 2.326
#define STATS_Z99 2.576

typedef struct Stat {
  uint64_t cardinality;
  uint64_t weight;
  double mean;
  double sum_of_mean_differences_squared;
} Stat;

Stat *create_stat();
void destroy_stat(Stat *stat);
void reset_stat(Stat *stat);
Stat *copy_stat(const Stat *stat);
void push(Stat *stat, double value, uint64_t weight);
void push_stat(Stat *stat_1, Stat *stat_2);
uint64_t get_cardinality(const Stat *stat);
uint64_t get_weight(const Stat *stat);
double get_mean(const Stat *stat);
double get_variance(const Stat *stat);
double get_stdev(const Stat *stat);
double get_standard_error(const Stat *stat, double m);
int round_to_nearest_int(double a);
void combine_stats(Stat **stats, int number_of_stats, Stat *combined_stat);

#endif