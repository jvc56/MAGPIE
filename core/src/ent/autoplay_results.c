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

int get_total_games(const AutoplayResults *autoplay_results) {
  return autoplay_results->total_games;
}

int get_p1_wins(const AutoplayResults *autoplay_results) {
  return autoplay_results->p1_wins;
}

int get_p1_losses(const AutoplayResults *autoplay_results) {
  return autoplay_results->p1_losses;
}

int get_p1_ties(const AutoplayResults *autoplay_results) {
  return autoplay_results->p1_ties;
}

int get_p1_firsts(const AutoplayResults *autoplay_results) {
  return autoplay_results->p1_firsts;
}

Stat *get_p1_score(const AutoplayResults *autoplay_results) {
  return autoplay_results->p1_score;
}

Stat *get_p2_score(const AutoplayResults *autoplay_results) {
  return autoplay_results->p2_score;
}

// Setter functions
void increment_total_games(AutoplayResults *autoplay_results) {
  autoplay_results->total_games++;
}

void increment_p1_wins(AutoplayResults *autoplay_results) {
  autoplay_results->p1_wins++;
}

void increment_p1_losses(AutoplayResults *autoplay_results) {
  autoplay_results->p1_losses++;
}

void increment_p1_ties(AutoplayResults *autoplay_results) {
  autoplay_results->p1_ties++;
}

void increment_p1_firsts(AutoplayResults *autoplay_results) {
  autoplay_results->p1_firsts++;
}

void increment_p1_score(AutoplayResults *autoplay_results, int score) {
  push(autoplay_results->p1_score, (double)score, 1);
}

void increment_p2_score(AutoplayResults *autoplay_results, int score) {
  push(autoplay_results->p2_score, (double)score, 1);
}

void reset_autoplay_results(AutoplayResults *autoplay_results) {
  autoplay_results->total_games = 0;
  autoplay_results->p1_wins = 0;
  autoplay_results->p1_losses = 0;
  autoplay_results->p1_ties = 0;
  autoplay_results->p1_firsts = 0;
  reset_stat(autoplay_results->p1_score);
  reset_stat(autoplay_results->p2_score);
}

AutoplayResults *create_autoplay_results() {
  AutoplayResults *autoplay_results = malloc_or_die(sizeof(AutoplayResults));
  autoplay_results->p1_score = create_stat();
  autoplay_results->p2_score = create_stat();
  reset_autoplay_results(autoplay_results);
  return autoplay_results;
}

void destroy_autoplay_results(AutoplayResults *autoplay_results) {
  destroy_stat(autoplay_results->p1_score);
  destroy_stat(autoplay_results->p2_score);
  free(autoplay_results);
}

void add_autoplay_results(const AutoplayResults *result_to_add,
                          AutoplayResults *result_to_be_updated) {
  // Stats are combined elsewhere
  result_to_be_updated->p1_firsts += result_to_add->p1_firsts;
  result_to_be_updated->p1_wins += result_to_add->p1_wins;
  result_to_be_updated->p1_losses += result_to_add->p1_losses;
  result_to_be_updated->p1_ties += result_to_add->p1_ties;
  result_to_be_updated->total_games += result_to_add->total_games;
}
