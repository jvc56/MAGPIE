#ifndef AUTOPLAY_H
#define AUTOPLAY_H

#include "config.h"
#include "stats.h"
#include "thread_control.h"

typedef struct AutoplayResults {
  pthread_mutex_t update_results_mutex;
  int p1_wins;
  int p1_losses;
  int p1_ties;
  int p1_firsts;
  Stat *p1_score;
  Stat *p2_score;
  int total_games;
} AutoplayResults;

void autoplay(Config *config, ThreadControl *thread_control);

#endif