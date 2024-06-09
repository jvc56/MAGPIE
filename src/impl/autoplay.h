#ifndef AUTOPLAY_H
#define AUTOPLAY_H

#include "../def/autoplay_defs.h"

#include "../ent/autoplay_results.h"
#include "config.h"

typedef struct AutoplayArgs {
  int max_iterations;
  bool use_game_pairs;
  uint64_t seed;
  GameArgs *game_args;
  ThreadControl *thread_control;
} AutoplayArgs;

autoplay_status_t autoplay(const AutoplayArgs *args,
                           AutoplayResults *autoplay_results);

#endif
