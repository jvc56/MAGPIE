#ifndef INFERENCE_H
#define INFERENCE_H

#include "../def/inference_defs.h"
#include "../ent/game.h"
#include "../ent/game_history.h"
#include "../ent/inference_results.h"
#include "../ent/rack.h"
#include "../ent/thread_control.h"
#include "../util/io_util.h"

typedef struct InferenceArgs {
  bool use_game_history;
  GameHistory *game_history;
  int target_index;
  Equity target_score;
  int target_num_exch;
  int move_capacity;
  Equity equity_margin;
  Rack *target_played_tiles;
  Rack *target_known_rack;
  Rack *nontarget_known_rack;
  const Game *game;
  int num_threads;
  int print_interval;
  ThreadControl *thread_control;
} InferenceArgs;

void infer(InferenceArgs *args, InferenceResults *results,
           ErrorStack *error_stack);

#endif