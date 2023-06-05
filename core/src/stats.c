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

void push(Stat * stat, double value, int value_weight) {
    stat->cardinality++;
    stat->weight += value_weight;
    double old_mean = stat->mean;
    double value_minus_old_mean = ((double)value) - old_mean;
    stat->mean = old_mean + (((double)value_weight)/stat->weight) * value_minus_old_mean;
    stat->sum_of_mean_differences_squared = stat->sum_of_mean_differences_squared +
      ((double)value_weight) * value_minus_old_mean * (((double)value) - stat->mean);
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
