#ifndef SIM_H
#define SIM_H
#include <pthread.h>

#include "game.h"
#include "move.h"
#include "rack.h"
#include "stats.h"
#include "winpct.h"

#define THREAD_CONTROL_IDLE 0
#define THREAD_CONTROL_RUNNING 1
#define THREAD_CONTROL_SHOULD_STOP 2

typedef struct SimmedPlay {
  Move *move;
  Stat **score_stat;
  Stat **bingo_stat;
  Stat *equity_stat;
  Stat *leftover_stat;
  Stat *win_pct_stat;
  int ignore;
  int multithreaded;
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
  int iteration_count;
  int threads;
  int num_simmed_plays;

  // Actually bool:
  int simming;
  ThreadControl **thread_control;

  SimmedPlay **simmed_plays;
  Rack *known_opp_rack;
  WinPct *win_pcts;
};

struct threadcontrol {
  Simmer *simmer;
  int thread_number;
  int status;
  pthread_t thread;
};

Simmer *create_simmer(Config *config, Game *game);
void destroy_simmer(Simmer *simmer);
void prepare_simmer(Simmer *simmer, int plies, int threads, Move **plays, int num_plays,
                    Rack *known_opp_rack);
void print_sim_stats(Simmer *simmer);
void set_stopping_condition(Simmer *simmer, int sc);
void simulate(Simmer *simmer);
void sim_single_iteration(Simmer *simmer, int plies, int thread);
void sort_plays_by_win_rate(SimmedPlay **simmed_plays, int num_simmed_plays);
void stop_simming(Simmer *simmer);

#endif
