#ifndef SIMMER_H
#define SIMMER_H

#include "../def/simmer_defs.h"

#include "../ent/game.h"
#include "../ent/move.h"
#include "../ent/sim_results.h"
#include "../ent/thread_control.h"

typedef struct SimArgs {
  int max_iterations;
  int num_plies;
  double stop_cond_pct;
  uint64_t seed;
  const Game *game;
  const MoveList *move_list;
  Rack *known_opp_rack;
  WinPct *win_pcts;
  ThreadControl *thread_control;
} SimArgs;

sim_status_t simulate(const SimArgs *args, SimResults *sim_results);
double percentage_to_z(double percentile);

#endif
