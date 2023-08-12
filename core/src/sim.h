#ifndef SIM_H
#define SIM_H
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

#include "game.h"
#include "move.h"
#include "rack.h"
#include "stats.h"
#include "thread_control.h"
#include "winpct.h"

#define THREAD_CONTROL_IDLE 0
#define THREAD_CONTROL_RUNNING 1
#define THREAD_CONTROL_SHOULD_STOP 2
#define THREAD_CONTROL_EXITING 3

#define PLAYS_NOT_SIMILAR 0
#define PLAYS_SIMILAR 1
#define UNINITIALIZED_SIMILARITY 2
#define PLAYS_IDENTICAL 3

typedef struct SimmedPlay {
  Move *move;
  Stat **score_stat;
  Stat **bingo_stat;
  Stat *equity_stat;
  Stat *leftover_stat;
  Stat *win_pct_stat;
  int ignore;
  int play_id;
  pthread_mutex_t mutex;
} SimmedPlay;

typedef struct Simmer {
  int initial_spread;
  int max_plies;
  int initial_player;
  int iteration_count;
  pthread_mutex_t iteration_count_mutex;
  int max_iterations;
  int num_simmed_plays;
  struct timespec start_time;

  int stopping_condition;
  int threads;

  SimmedPlay **simmed_plays;
  pthread_mutex_t simmed_plays_mutex;

  Rack *known_opp_rack;
  Rack *similar_plays_rack;
  WinPct *win_pcts;

  int *play_similarity_cache;

  ThreadControl *thread_control;
  FILE *outfile;
} Simmer;

typedef struct SimmerWorker {
  int thread_index;
  Game *game;
  Rack *rack_placeholder;
  Simmer *simmer;
} SimmerWorker;

Simmer *create_simmer(Config *config, FILE *outfile);
void destroy_simmer(Simmer *simmer);
void join_threads(Simmer *simmer);
int plays_are_similar(Simmer *simmer, SimmedPlay *m1, SimmedPlay *m2);
void print_ucgi_stats(Simmer *simmer, Game *game, int print_best_play);
void simulate(ThreadControl *thread_control, Simmer *simmer, Game *game,
              Rack *known_opp_rack, int plies, int threads, int num_plays,
              int max_iterations, int stopping_condition);
void sort_plays_by_win_rate(SimmedPlay **simmed_plays, int num_simmed_plays);

#endif
