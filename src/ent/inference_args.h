#ifndef INFERENCE_ARGS_H
#define INFERENCE_ARGS_H

#include "../def/inference_defs.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/game_history.h"
#include "../ent/inference_results.h"
#include "../ent/move.h"
#include "../ent/rack.h"
#include "../ent/thread_control.h"
#include "../util/io_util.h"

typedef struct InferenceArgs {
  bool use_game_history;
  bool use_inference_cutoff_optimization;
  // Value the racks with the board's census play scores, built once, instead
  // of a move generation for each rack, where that gives the same values (see
  // inference_can_use_census in inference.c). The racks inferred are the same
  // either way.
  bool use_census_play_scores;
  GameHistory *game_history;
  int target_index;
  Equity target_score;
  int target_num_exch;
  int leave_list_capacity;
  Equity equity_margin;
  Equity temperature;
  const Move *target_move;
  Rack *target_played_tiles;
  Rack *target_known_rack;
  Rack *nontarget_known_rack;
  const Game *game;
  int num_threads;
  int parent_worker_thread_index;
  int print_interval;
  ThreadControl *thread_control;
} InferenceArgs;

static inline void
infer_args_fill(InferenceArgs *args, int leave_list_capacity, Equity eq_margin,
                GameHistory *game_history, const Game *game, int num_threads,
                int parent_worker_thread_index, int print_interval,
                ThreadControl *thread_control, bool use_game_history,
                bool use_inference_cutoff_optimization, int target_index,
                Equity target_score, int target_num_exch,
                Rack *target_played_tiles, Rack *target_known_rack,
                Rack *nontarget_known_rack) {
  args->target_index = target_index;
  args->target_score = target_score;
  args->target_num_exch = target_num_exch;
  args->leave_list_capacity = leave_list_capacity;
  args->equity_margin = eq_margin;
  args->temperature = 0;
  args->target_move = NULL;
  args->target_played_tiles = target_played_tiles;
  args->target_known_rack = target_known_rack;
  args->nontarget_known_rack = nontarget_known_rack;
  args->use_game_history = use_game_history;
  args->use_inference_cutoff_optimization = use_inference_cutoff_optimization;
  args->use_census_play_scores = true;
  args->game_history = game_history;
  args->game = game;
  args->num_threads = num_threads;
  args->parent_worker_thread_index = parent_worker_thread_index;
  args->print_interval = print_interval;
  args->thread_control = thread_control;
}

#endif