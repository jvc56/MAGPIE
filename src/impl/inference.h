#ifndef INFERENCE_H
#define INFERENCE_H

#include "../def/inference_defs.h"

#include "../ent/game.h"
#include "../ent/inference_results.h"
#include "../ent/rack.h"
#include "../ent/thread_control.h"

typedef struct InferenceArgs {
  int target_index;
  int target_score;
  int target_num_exch;
  int move_capacity;
  float equity_margin;
  Rack *target_played_tiles;
  const Game *game;
  ThreadControl *thread_control;
} InferenceArgs;

inference_status_t infer(InferenceArgs *args, InferenceResults *results);

#endif