#ifndef AUTOPLAY_H
#define AUTOPLAY_H

#include <stdbool.h>

#include "../def/autoplay_defs.h"

#include "../ent/autoplay_results.h"
#include "../ent/game.h"
#include "../ent/thread_control.h"

typedef struct AutoplayArgs {
  int gens;
  int games_per_gen;
  int target_min_leave_count;
  int force_draw_start;
  int max_force_draw_turn;
  bool use_game_pairs;
  bool human_readable;
  autoplay_t type;
  const char *data_paths;
  GameArgs *game_args;
  ThreadControl *thread_control;
} AutoplayArgs;

autoplay_status_t autoplay(const AutoplayArgs *args,
                           AutoplayResults *autoplay_results);

#endif
