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
  // Nerfed-player target ratings; 0 disables nerfing for that player.
  int p1_nerf_rating;
  int p2_nerf_rating;
  // Phony/challenge flow: players play from believed lexicons (their game
  // KWGs, e.g. CSW24PH1400), plays are adjudicated against the real
  // lexicon, and nerfed opponents challenge at fitted rates. The
  // challenge rule follows the real lexicon family: CSW/OSW = 5 points
  // per word, otherwise double challenge (challenger loses a turn).
  bool nerf_phony;
  // Validation logging: append pre-endgame positions (bag 1-6) and the
  // chosen move to peglog_w<worker>.tsv (CGP, UCGI move, mover rating).
  bool peg_log;
  SimArgs p1_sim_args;
  SimArgs p2_sim_args;
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
