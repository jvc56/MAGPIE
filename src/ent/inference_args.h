#ifndef INFERENCE_ARGS_H
#define INFERENCE_ARGS_H

#include "../def/inference_defs.h"
#include "../ent/equity.h"
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

static inline void
infer_args_fill(InferenceArgs *args, int num_plays, Equity eq_margin,
                GameHistory *game_history, const Game *game, int num_threads,
                int print_interval, ThreadControl *thread_control,
                bool use_game_history, int target_index, Equity target_score,
                int target_num_exch, Rack *target_played_tiles,
                Rack *target_known_rack, Rack *nontarget_known_rack) {
  args->target_index = target_index;
  args->target_score = target_score;
  args->target_num_exch = target_num_exch;
  args->move_capacity = num_plays;
  args->equity_margin = eq_margin;
  args->target_played_tiles = target_played_tiles;
  args->target_known_rack = target_known_rack;
  args->nontarget_known_rack = nontarget_known_rack;
  args->use_game_history = use_game_history;
  args->game_history = game_history;
  args->game = game;
  args->num_threads = num_threads;
  args->print_interval = print_interval;
  args->thread_control = thread_control;
}

#endif