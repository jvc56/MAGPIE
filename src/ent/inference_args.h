#ifndef INFERENCE_ARGS_H
#define INFERENCE_ARGS_H

#include "../def/inference_defs.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/game_history.h"
#include "../ent/inference_results.h"
#include "../ent/rack.h"
#include "../ent/thread_control.h"
#include "../impl/move_gen_cache.h"
#include "../util/io_util.h"

typedef struct InferenceArgs {
  bool use_game_history;
  bool use_inference_cutoff_optimization;
  GameHistory *game_history;
  int target_index;
  Equity target_score;
  int target_num_exch;
  int leave_list_capacity;
  Equity equity_margin;
  Rack *target_played_tiles;
  Rack *target_known_rack;
  Rack *nontarget_known_rack;
  const Game *game;
  int num_threads;
  MoveGenCache *movegen_cache;
  int movegen_start_index;
  int print_interval;
  ThreadControl *thread_control;
} InferenceArgs;

static inline void
infer_args_fill(InferenceArgs *args, int leave_list_capacity, Equity eq_margin,
                GameHistory *game_history, const Game *game, int num_threads,
                MoveGenCache *movegen_cache, int movegen_start_index,
                int print_interval, ThreadControl *thread_control,
                bool use_game_history, bool use_inference_cutoff_optimization,
                int target_index, Equity target_score, int target_num_exch,
                Rack *target_played_tiles, Rack *target_known_rack,
                Rack *nontarget_known_rack) {
  args->target_index = target_index;
  args->target_score = target_score;
  args->target_num_exch = target_num_exch;
  args->leave_list_capacity = leave_list_capacity;
  args->equity_margin = eq_margin;
  args->target_played_tiles = target_played_tiles;
  args->target_known_rack = target_known_rack;
  args->nontarget_known_rack = nontarget_known_rack;
  args->use_game_history = use_game_history;
  args->use_inference_cutoff_optimization = use_inference_cutoff_optimization;
  args->game_history = game_history;
  args->game = game;
  args->num_threads = num_threads;
  args->movegen_cache = movegen_cache;
  args->movegen_start_index = movegen_start_index;
  args->print_interval = print_interval;
  args->thread_control = thread_control;
}

#endif