#ifndef AUTOPLAY_H
#define AUTOPLAY_H

#include "../def/autoplay_defs.h"
#include "../ent/autoplay_results.h"
#include "../ent/game.h"
#include "../ent/sim_args.h"
#include "../ent/thread_control.h"
#include "../util/io_util.h"
#include <stdbool.h>

typedef struct GameStringOptions GameStringOptions;

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
  multi_threading_mode_t multi_threading_mode;
  double cutoff;
  SimArgs p1_sim_args;
  SimArgs p2_sim_args;
  // outcome_model training-data dump. NULL if disabled. When non-NULL,
  // every tile-placement move emits one CSV row of (features, eventual
  // win/spread) at recording-moment on-turn POV. See outcome_recorder.h.
  const char *outcome_dump_path;
  int outcome_bingo_samples; // bingo_prob sample count per side (default 14)
} AutoplayArgs;

void autoplay(const AutoplayArgs *args, AutoplayResults *autoplay_results,
              ErrorStack *error_stack);

// Benchmark instrumentation: returns accumulated sim iteration count across
// all autoplay sims since process start (or last reset). Used by simbench.
uint64_t autoplay_get_total_sim_iterations(void);
void autoplay_reset_total_sim_iterations(void);

// Benchmark mode: when enabled, the sim still runs at every turn (for timing
// and iteration counts) but autoplay plays the top-equity static move
// instead of the sim's selection. This produces a deterministic game
// trajectory so RIT/BAI variants can be compared over identical positions.
void autoplay_set_bench_static_move(bool enabled);
bool autoplay_get_bench_static_move(void);

#endif
