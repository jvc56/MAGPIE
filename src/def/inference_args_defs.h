#ifndef INFERENCE_ARGS_DEFS_H
#define INFERENCE_ARGS_DEFS_H

#include "../ent/game.h"
#include "../ent/game_history.h"
#include "../ent/move.h"
#include "../ent/rack.h"
#include "../ent/thread_control.h"
#include "inference_defs.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct InferenceArgs {
  Game *game;
  GameHistory *game_history;
  int target_index;
  Equity target_score;
  int target_num_exch;
  int move_capacity;
  Equity equity_margin;
  Rack *target_played_tiles;
  Rack *target_known_rack;
  Rack *nontarget_known_rack;
  Move target_actual_move; // Changed from pointer to direct struct
  bool use_game_history;
  bool skip_return_racks_to_bag;
  ThreadControl *thread_control;
  int num_threads;
  int print_interval;
  int movegen_thread_offset;
} InferenceArgs;

#endif // INFERENCE_ARGS_DEFS_H
