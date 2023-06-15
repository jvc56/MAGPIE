#include <stdlib.h>
#include <math.h>

#include "stats.h"

void reset_stat(Stat * stat) {
    stat->cardinality = 0;
    stat->weight = 0;
    stat->mean = 0;
    stat->sum_of_mean_differences_squared = 0;
}

Stat * create_stat() {
	Stat * stat = malloc(sizeof(Stat));
    reset_stat(stat);
	return stat;
}

void destroy_stat(Stat * stat) {
    free(stat);
}

Stat * copy_stat(Stat * original_stat) {
    Stat * copy_stat = create_stat();
    copy_stat->cardinality = original_stat->cardinality;
    copy_stat->weight = original_stat->weight;
    copy_stat->sum_of_mean_differences_squared = original_stat->sum_of_mean_differences_squared;
    copy_stat->mean = original_stat->mean;
    return copy_stat;
}

void push_with_cardinality(Stat * stat, double value, uint64_t value_weight, uint64_t cardinality) {
    stat->cardinality += cardinality;
    stat->weight += value_weight;
    double old_mean = stat->mean;
    double value_minus_old_mean = ((double)value) - old_mean;
    stat->mean = old_mean + (((double)value_weight)/stat->weight) * value_minus_old_mean;
    stat->sum_of_mean_differences_squared = stat->sum_of_mean_differences_squared +
      ((double)value_weight) * value_minus_old_mean * (((double)value) - stat->mean);
}

void push(Stat * stat, double value, uint64_t value_weight) {
    push_with_cardinality(stat, value, value_weight, 1);
}

uint64_t cardinality(Stat * stat) {
    return stat->cardinality;
}

uint64_t weight(Stat * stat) {
    return stat->weight;
}

double mean(Stat * stat) {
    return stat->mean;
}

void push_stat(Stat * stat_1, Stat * stat_2) {
    push_with_cardinality(stat_1, mean(stat_2), weight(stat_2), cardinality(stat_2));
}

double variance(Stat * stat) {
    if (stat->weight <= 1) {
        return 0.0;
    }
    // Use population variance since the stat
    // has data for the entire probability space.
    return stat->sum_of_mean_differences_squared / (((double)stat->weight));
}

double stdev(Stat * stat) {
    return sqrt(variance(stat));
}
