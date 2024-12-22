#ifndef AUTOPLAY_H
#define AUTOPLAY_H

#include <stdbool.h>

#include "../def/autoplay_defs.h"

#include "../ent/autoplay_results.h"
#include "../ent/game.h"
#include "../ent/thread_control.h"

#include "../impl/config.h"

typedef struct AutoplayArgs {
  const char *num_games_or_min_leave_targets;
  int games_before_force_draw_start;
  bool use_game_pairs;
  bool human_readable;
  autoplay_t type;
  const char *data_paths;
  GameArgs *game_args;
  ThreadControl *thread_control;
} AutoplayArgs;

autoplay_status_t autoplay(const AutoplayArgs *args, const Config *config,
                           AutoplayResults *autoplay_results);

#endif
