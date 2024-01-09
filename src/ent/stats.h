#ifndef STATS_H
#define STATS_H

#include <stdint.h>

typedef struct Stat Stat;

Stat *stat_create();
void stat_destroy(Stat *stat);
void stat_reset(Stat *stat);

uint64_t stat_get_cardinality(const Stat *stat);
uint64_t stat_get_weight(const Stat *stat);
double stat_get_mean(const Stat *stat);
double stat_get_variance(const Stat *stat);
double stat_get_stdev(const Stat *stat);
double stat_get_stderr(const Stat *stat, double m);

void stat_push(Stat *stat, double value, uint64_t weight);

void stats_combine(Stat **stats, int number_of_stats, Stat *combined_stat);

#endif