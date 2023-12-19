#ifndef AUTOPLAY_RESULTS_H
#define AUTOPLAY_RESULTS_H

#include "stats.h"

struct AutoplayResults;
typedef struct AutoplayResults AutoplayResults;

AutoplayResults *create_autoplay_results();
void destroy_autoplay_results(AutoplayResults *autoplay_results);

int get_total_games(const AutoplayResults *autoplay_results);
int get_p1_wins(const AutoplayResults *autoplay_results);
int get_p1_losses(const AutoplayResults *autoplay_results);
int get_p1_ties(const AutoplayResults *autoplay_results);
int get_p1_firsts(const AutoplayResults *autoplay_results);
Stat *get_p1_score(const AutoplayResults *autoplay_results);
Stat *get_p2_score(const AutoplayResults *autoplay_results);
void increment_total_games(AutoplayResults *autoplay_results);
void increment_p1_wins(AutoplayResults *autoplay_results);
void increment_p1_losses(AutoplayResults *autoplay_results);
void increment_p1_ties(AutoplayResults *autoplay_results);
void increment_p1_firsts(AutoplayResults *autoplay_results);
void increment_p1_score(AutoplayResults *autoplay_results, int score);
void increment_p2_score(AutoplayResults *autoplay_results, int score);

void add_autoplay_results(const AutoplayResults *result_to_add,
                          AutoplayResults *result_to_be_updated);
void reset_autoplay_results(AutoplayResults *autoplay_results);
void create_or_reset_autoplay_results(AutoplayResults **autoplay_results);

#endif