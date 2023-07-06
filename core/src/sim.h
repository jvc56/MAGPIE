#ifndef SIM_H
#define SIM_H

#include "game.h"
#include "move.h"
#include "rack.h"
#include "stats.h"
#include "winpct.h"

typedef struct SimmedPlay {
  Move *move;
  Stat **score_stat;
  Stat **bingo_stat;
  Stat *equity_stat;
  Stat *leftover_stat;
  Stat *win_pct_stat;
  int ignore;
} SimmedPlay;

typedef struct Simmer {
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

  SimmedPlay **simmed_plays;
  Rack *known_opp_rack;
  WinPct *win_pcts;
} Simmer;

Simmer *create_simmer(Config *config, Game *game);
void destroy_simmer(Simmer *simmer);
void prepare_simmer(Simmer *simmer, int plies, Move **plays, int num_plays,
                    Rack *known_opp_rack);
void print_sim_stats(Simmer *simmer);
void set_stopping_condition(Simmer *simmer, int sc);
void set_threads(Simmer *simmer, int t);
void simulate(Simmer *simmer);
void sim_single_iteration(Simmer *simmer, int plies, int thread);

#endif
