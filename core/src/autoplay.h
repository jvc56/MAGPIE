#ifndef AUTOPLAY_H
#define AUTOPLAY_H

#include "config.h"
#include "game.h"
#include "stats.h"

typedef struct AutoplayResults {
  int total_games;
  int p1_wins;
  int p1_losses;
  int p1_ties;
  int p1_firsts;
  Stat *p1_score;
  Stat *p2_score;
} AutoplayResults;

void autoplay(Config *config, Game *game, AutoplayResults *autoplay_results);
AutoplayResults *create_autoplay_results();
void destroy_autoplay_results(AutoplayResults *autoplay_results);

#endif