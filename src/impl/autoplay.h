#ifndef AUTOPLAY_H
#define AUTOPLAY_H

#include "../def/autoplay_defs.h"
#include "../def/bai_defs.h"
#include "../ent/autoplay_results.h"
#include "../ent/game.h"
#include "../ent/thread_control.h"
#include "../ent/win_pct.h"
#include "../util/io_util.h"
#include <stdbool.h>

typedef struct GameStringOptions GameStringOptions;
typedef struct GameArgs GameArgs;

typedef struct AutoplayArgs {
  const char *num_games_or_min_rack_targets;
  int games_before_force_draw_start;
  bool use_game_pairs;
  bool human_readable;
  bool print_boards;
  autoplay_t type;
  const char *data_paths;
  GameArgs *game_args;
  int num_threads;
  int print_interval;
  uint64_t seed;
  ThreadControl *thread_control;
  const GameStringOptions *game_string_options;
  // Per-player simulation parameters
  int p1_sim_plies;
  int p2_sim_plies;
  double p1_stop_cond_pct;
  double p2_stop_cond_pct;
  uint64_t p1_max_iterations;
  uint64_t p2_max_iterations;
  uint64_t p1_min_play_iterations;
  uint64_t p2_min_play_iterations;
  bool multi_threading_mode;
  // BAI parameters
  bai_sampling_rule_t sampling_rule;
  bai_threshold_t threshold;
  double cutoff;
  int max_num_display_plays;
  uint64_t time_limit_seconds;
  WinPct *win_pcts;
} AutoplayArgs;

void autoplay(const AutoplayArgs *args, AutoplayResults *autoplay_results,
              ErrorStack *error_stack);

#endif
