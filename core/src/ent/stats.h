#ifndef STATS_H
#define STATS_H

#include <stdint.h>

struct Stat;
typedef struct Stat Stat;

Stat *create_stat();
void destroy_stat(Stat *stat);
void reset_stat(Stat *stat);
Stat *stat_duplicate(const Stat *stat);
void push(Stat *stat, double value, uint64_t weight);
uint64_t get_cardinality(const Stat *stat);
uint64_t get_weight(const Stat *stat);
double get_mean(const Stat *stat);
double get_variance(const Stat *stat);
double get_stdev(const Stat *stat);
double get_standard_error(const Stat *stat, double m);
int round_to_nearest_int(double a);
void combine_stats(Stat **stats, int number_of_stats, Stat *combined_stat);

#endif