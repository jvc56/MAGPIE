#ifndef STATS_H
#define STATS_H

#include <stdint.h>

typedef struct Stat {
    uint64_t cardinality;
    uint64_t weight;
    double mean;
    double sum_of_mean_differences_squared;
} Stat;

Stat * create_stat();
void destroy_stat(Stat * stat);
void reset_stat(Stat * stat);
void push(Stat * stat, double value, int weight);
uint64_t cardinality(Stat * stat);
uint64_t weight(Stat * stat);
double mean(Stat * stat);
double variance(Stat * stat);
double stdev(Stat * stat);

#endif