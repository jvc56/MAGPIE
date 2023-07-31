#ifndef SIM_H
#define SIM_H
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

#include "game.h"
#include "move.h"
#include "rack.h"
#include "stats.h"
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
  int multithreaded;
  int play_id;
  pthread_mutex_t mutex;
} SimmedPlay;

typedef struct threadcontrol ThreadControl;
typedef struct simmer Simmer;

typedef void (*callback_fn)(void);

struct simmer {
  Game *game; // contains a generator already.
  Game **game_copies;
  Rack **rack_placeholders;

  int initial_spread;
  int max_plies;
  int initial_player;
  atomic_int iteration_count;
  atomic_int node_count;
  int threads;
  int num_simmed_plays;
  int ucgi_mode;
  struct timespec start_time;

  ThreadControl **thread_control;
  int stopping_condition;

  SimmedPlay **simmed_plays;
  pthread_mutex_t simmed_plays_mutex;

  Rack *known_opp_rack;
  WinPct *win_pcts;

  int *play_similarity_cache;
  callback_fn endsim_callback;
};

struct threadcontrol {
  Simmer *simmer;
  int thread_number;
  int last_iteration_ct;
  volatile int status; // another thread can change this, so i think this needs
                       // to be volatile.
  pthread_t thread;
};

Simmer *create_simmer(Config *config, Game *game);
void blocking_simulate(Simmer *simmer);
void destroy_simmer(Simmer *simmer);
void join_threads(Simmer *simmer);
int plays_are_similar(Simmer *simmer, SimmedPlay *m1, SimmedPlay *m2);
void prepare_simmer(Simmer *simmer, int plies, int threads, Move **plays,
                    int num_plays, Rack *known_opp_rack);
void print_ucgi_stats(Simmer *simmer, int print_best_play);
void print_sim_stats(Simmer *simmer);
void set_stopping_condition(Simmer *simmer, int sc);
void set_ucgi_mode(Simmer *simmer, int mode);
void set_endsim_callback(Simmer *, callback_fn);
void simulate(Simmer *simmer);
void sim_single_iteration(Simmer *simmer, int plies, int thread);
void sort_plays_by_win_rate(SimmedPlay **simmed_plays, int num_simmed_plays);
void stop_simming(Simmer *simmer);

#endif
