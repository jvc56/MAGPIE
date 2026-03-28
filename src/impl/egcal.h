#ifndef EGCAL_H
#define EGCAL_H

#include "../ent/game.h"
#include "../ent/thread_control.h"
#include "../util/io_util.h"
#include <stdint.h>

typedef struct EgcalArgs {
  const GameArgs *game_args;
  const char *data_paths;
  const char *lexicon_name;
  ThreadControl *thread_control;
  uint64_t num_positions;
  int num_threads;
  int print_interval;
  uint64_t seed;
} EgcalArgs;

void egcal(const EgcalArgs *args, ErrorStack *error_stack);

#endif
