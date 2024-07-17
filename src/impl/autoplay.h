#ifndef AUTOPLAY_H
#define AUTOPLAY_H

#include <stdbool.h>

#include "../def/autoplay_defs.h"

#include "../ent/autoplay_results.h"
#include "../ent/game.h"
#include "../ent/thread_control.h"

typedef struct AutoplayArgs {
  int gens;
  int max_iterations;
  int max_force_draw_turn;
  bool create_leaves;
  bool use_game_pairs;
  autoplay_t type;
  GameArgs *game_args;
  ThreadControl *thread_control;
} AutoplayArgs;

autoplay_status_t autoplay(const AutoplayArgs *args,
                           AutoplayResults *autoplay_results);

#endif
