#include "autoplay_results.h"
#include "stats.h"

#include "../util/util.h"

struct AutoplayResults {
  int total_games;
  int p1_wins;
  int p1_losses;
  int p1_ties;
  int p1_firsts;
  Stat *p1_score;
  Stat *p2_score;
};

void autoplay_results_reset(AutoplayResults *autoplay_results) {
  autoplay_results->total_games = 0;
  autoplay_results->p1_wins = 0;
  autoplay_results->p1_losses = 0;
  autoplay_results->p1_ties = 0;
  autoplay_results->p1_firsts = 0;
  stat_reset(autoplay_results->p1_score);
  stat_reset(autoplay_results->p2_score);
}

AutoplayResults *autoplay_results_create() {
  AutoplayResults *autoplay_results = malloc_or_die(sizeof(AutoplayResults));
  autoplay_results->p1_score = stat_create();
  autoplay_results->p2_score = stat_create();
  autoplay_results_reset(autoplay_results);
  return autoplay_results;
}

void autoplay_results_destroy(AutoplayResults *autoplay_results) {
  stat_destroy(autoplay_results->p1_score);
  stat_destroy(autoplay_results->p2_score);
  free(autoplay_results);
}

int autoplay_results_get_games(const AutoplayResults *autoplay_results) {
  return autoplay_results->total_games;
}

int autoplay_results_get_p1_wins(const AutoplayResults *autoplay_results) {
  return autoplay_results->p1_wins;
}

int autoplay_results_get_p1_losses(const AutoplayResults *autoplay_results) {
  return autoplay_results->p1_losses;
}

int autoplay_results_get_p1_ties(const AutoplayResults *autoplay_results) {
  return autoplay_results->p1_ties;
}

int autoplay_results_get_p1_firsts(const AutoplayResults *autoplay_results) {
  return autoplay_results->p1_firsts;
}

Stat *autoplay_results_get_p1_score(const AutoplayResults *autoplay_results) {
  return autoplay_results->p1_score;
}

Stat *autoplay_results_get_p2_score(const AutoplayResults *autoplay_results) {
  return autoplay_results->p2_score;
}

void autoplay_results_increment_total_games(AutoplayResults *autoplay_results) {
  autoplay_results->total_games++;
}

void autoplay_results_increment_p1_wins(AutoplayResults *autoplay_results) {
  autoplay_results->p1_wins++;
}

void autoplay_results_increment_p1_losses(AutoplayResults *autoplay_results) {
  autoplay_results->p1_losses++;
}

void autoplay_results_increment_p1_ties(AutoplayResults *autoplay_results) {
  autoplay_results->p1_ties++;
}

void autoplay_results_increment_p1_firsts(AutoplayResults *autoplay_results) {
  autoplay_results->p1_firsts++;
}

void autoplay_results_increment_p1_score(AutoplayResults *autoplay_results,
                                         int score) {
  stat_push(autoplay_results->p1_score, (double)score, 1);
}

void autoplay_results_increment_p2_score(AutoplayResults *autoplay_results,
                                         int score) {
  stat_push(autoplay_results->p2_score, (double)score, 1);
}

void autoplay_results_add(const AutoplayResults *result_to_add,
                          AutoplayResults *result_to_be_updated) {
  // Stats are combined elsewhere
  result_to_be_updated->p1_firsts += result_to_add->p1_firsts;
  result_to_be_updated->p1_wins += result_to_add->p1_wins;
  result_to_be_updated->p1_losses += result_to_add->p1_losses;
  result_to_be_updated->p1_ties += result_to_add->p1_ties;
  result_to_be_updated->total_games += result_to_add->total_games;
}
