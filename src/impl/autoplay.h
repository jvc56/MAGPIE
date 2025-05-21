#ifndef AUTOPLAY_H
#define AUTOPLAY_H

#include <stdbool.h>

#include "../def/autoplay_defs.h"

#include "../ent/autoplay_results.h"
#include "../ent/game.h"
#include "../ent/thread_control.h"
#include "../util/error_stack.h"

typedef struct AutoplayArgs {
  const char *num_games_or_min_rack_targets;
  int games_before_force_draw_start;
  bool use_game_pairs;
  bool human_readable;
  autoplay_t type;
  const char *data_paths;
  GameArgs *game_args;
  ThreadControl *thread_control;
} AutoplayArgs;

void autoplay(const AutoplayArgs *args, AutoplayResults *autoplay_results,
              ErrorStack *error_stack);

#endif
