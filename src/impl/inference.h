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
  // If true, movegen will use early cutoff optimization during inference.
  // When a move is found that exceeds target_score + leave + equity_margin,
  // movegen terminates early since we know the leave is not recordable.
  bool use_cutoff_optimization;
} InferenceArgs;

void infer(InferenceArgs *args, InferenceResults *results,
           ErrorStack *error_stack);

// Debug functions for cutoff optimization statistics
void inference_reset_cutoff_stats(void);
void inference_get_cutoff_stats(uint64_t *total_calls, uint64_t *triggered);
void inference_print_cutoff_stats(void);

// Debug functions for exchange inference analysis
void inference_reset_exchange_stats(void);
void inference_get_exchange_stats(uint64_t *total, uint64_t *by_margin,
                                   uint64_t *by_match, uint64_t *by_empty,
                                   uint64_t *prunable);

#endif