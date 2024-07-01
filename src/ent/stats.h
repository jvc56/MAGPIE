#ifndef STATS_H
#define STATS_H

#include <stdbool.h>
#include <stdint.h>

typedef struct Stat Stat;

Stat *stat_create(bool mean_is_estimated);
void stat_destroy(Stat *stat);
void stat_reset(Stat *stat);

uint64_t stat_get_num_unique_samples(const Stat *stat);
uint64_t stat_get_num_samples(const Stat *stat);
double stat_get_mean(const Stat *stat);
double stat_get_variance(const Stat *stat);
double stat_get_stdev(const Stat *stat);
double stat_get_sem(const Stat *stat);
double stat_get_margin_of_error(const Stat *stat, double zval);
bool stats_is_greater_than(const Stat *stat1, const Stat *stat2, double zval);

void stat_push(Stat *stat, double value, uint64_t num_samples);

void stats_combine(Stat **stats, int number_of_stats, Stat *combined_stat);

#endif