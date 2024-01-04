#ifndef AUTOPLAY_RESULTS_H
#define AUTOPLAY_RESULTS_H

#include "stats.h"

struct AutoplayResults;
typedef struct AutoplayResults AutoplayResults;

AutoplayResults *autoplay_results_create();
void autoplay_results_destroy(AutoplayResults *autoplay_results);

int autoplay_results_get_games(const AutoplayResults *autoplay_results);
int autoplay_results_get_p1_wins(const AutoplayResults *autoplay_results);
int autoplay_results_get_p1_losses(const AutoplayResults *autoplay_results);
int autoplay_results_get_p1_ties(const AutoplayResults *autoplay_results);
int autoplay_results_get_p1_firsts(const AutoplayResults *autoplay_results);
Stat *autoplay_results_get_p1_score(const AutoplayResults *autoplay_results);
Stat *autoplay_results_get_p2_score(const AutoplayResults *autoplay_results);

void autoplay_results_increment_total_games(AutoplayResults *autoplay_results);
void autoplay_results_increment_p1_wins(AutoplayResults *autoplay_results);
void autoplay_results_increment_p1_losses(AutoplayResults *autoplay_results);
void autoplay_results_increment_p1_ties(AutoplayResults *autoplay_results);
void autoplay_results_increment_p1_firsts(AutoplayResults *autoplay_results);
void autoplay_results_increment_p1_score(AutoplayResults *autoplay_results,
                                         int score);
void autoplay_results_increment_p2_score(AutoplayResults *autoplay_results,
                                         int score);

void autoplay_results_add(const AutoplayResults *result_to_add,
                          AutoplayResults *result_to_be_updated);
void autoplay_results_reset(AutoplayResults *autoplay_results);

#endif