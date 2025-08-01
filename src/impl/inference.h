#ifndef INFERENCE_H
#define INFERENCE_H

#include "../def/inference_defs.h"
#include "../ent/game.h"
#include "../ent/inference_results.h"
#include "../ent/rack.h"
#include "../ent/thread_control.h"
#include "../util/io_util.h"

typedef struct InferenceArgs {
  int target_index;
  int target_score;
  int target_num_exch;
  int move_capacity;
  double equity_margin;
  Rack *target_played_tiles;
  const Game *game;
  ThreadControl *thread_control;
} InferenceArgs;

void infer(InferenceArgs *args, InferenceResults *results,
           ErrorStack *error_stack);

#endif