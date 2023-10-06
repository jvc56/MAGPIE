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

typedef enum {
  THREAD_CONTROL_IDLE,
  THREAD_CONTROL_RUNNING,
  THREAD_CONTROL_SHOULD_STOP,
  THREAD_CONTROL_EXITING,
} thread_control_status_t;

typedef enum {
  PLAYS_NOT_SIMILAR,
  PLAYS_SIMILAR,
  UNINITIALIZED_SIMILARITY,
  PLAYS_IDENTICAL,
} similar_plays_t;
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
  atomic_int node_count;
  ThreadControl *thread_control;
} Simmer;

typedef struct SimmerWorker {
  int thread_index;
  Game *game;
  Rack *rack_placeholder;
  Simmer *simmer;
} SimmerWorker;

Simmer *create_simmer(Config *config);
void destroy_simmer(Simmer *simmer);
int plays_are_similar(Simmer *simmer, SimmedPlay *m1, SimmedPlay *m2);
void simulate(ThreadControl *thread_control, Simmer *simmer, Game *game,
              Rack *known_opp_rack, int plies, int threads, int num_plays,
              int max_iterations, int stopping_condition,
              int static_search_only);
void sort_plays_by_win_rate(SimmedPlay **simmed_plays, int num_simmed_plays);

#endif
