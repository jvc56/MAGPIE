#ifndef SIM_H
#define SIM_H
#include <pthread.h>
#include <stdatomic.h>

#include "game.h"
#include "move.h"
#include "rack.h"
#include "stats.h"
#include "winpct.h"

#define THREAD_CONTROL_IDLE 0
#define THREAD_CONTROL_RUNNING 1
#define THREAD_CONTROL_SHOULD_STOP 2

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
  int multithreaded;
  int play_id;
  pthread_mutex_t mutex;
} SimmedPlay;

typedef struct threadcontrol ThreadControl;
typedef struct simmer Simmer;

struct simmer {
  Game *game; // contains a generator already.
  Game **game_copies;
  Rack **rack_placeholders;

  int initial_spread;
  int max_plies;
  int initial_player;
  atomic_int iteration_count;
  int threads;
  int num_simmed_plays;

  int simming;
  ThreadControl **thread_control;
  int stopping_condition;

  SimmedPlay **simmed_plays;
  pthread_mutex_t simmed_plays_mutex;

  Rack *known_opp_rack;
  WinPct *win_pcts;

  int *play_similarity_cache;
};

struct threadcontrol {
  Simmer *simmer;
  int thread_number;
  int status;
  pthread_t thread;
};

Simmer *create_simmer(Config *config, Game *game);
void blocking_simulate(Simmer *simmer);
void destroy_simmer(Simmer *simmer);
int plays_are_similar(Simmer *simmer, SimmedPlay *m1, SimmedPlay *m2);
void prepare_simmer(Simmer *simmer, int plies, int threads, Move **plays,
                    int num_plays, Rack *known_opp_rack);
void print_sim_stats(Simmer *simmer);
void set_stopping_condition(Simmer *simmer, int sc);
void simulate(Simmer *simmer);
void sim_single_iteration(Simmer *simmer, int plies, int thread);
void sort_plays_by_win_rate(SimmedPlay **simmed_plays, int num_simmed_plays);
void stop_simming(Simmer *simmer);

#endif
