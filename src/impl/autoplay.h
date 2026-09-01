#ifndef AUTOPLAY_H
#define AUTOPLAY_H

#include "../def/autoplay_defs.h"
#include "../ent/autoplay_results.h"
#include "../ent/game.h"
#include "../ent/sim_args.h"
#include "../ent/thread_control.h"
#include "../util/io_util.h"
#include "play_chooser.h"
#include <stdbool.h>

typedef struct GameStringOptions GameStringOptions;

typedef struct AutoplayArgs {
  const char *num_games_or_min_rack_targets;
  int games_before_force_draw_start;
  // Optional: racks that restrict which racks leavegen's RackList ever
  // selects as rare (see rack_list_create). Only meaningful with
  // AUTOPLAY_TYPE_LEAVE_GEN. num_forced_racks 0 means every rack is
  // eligible, as leavegen normally expects.
  const char *const *forced_racks;
  int num_forced_racks;
  // How many ranked plays the positions recorder reports per captured
  // position, when active. Sizes each static player's move list up front
  // (see autoplay_worker_create) and is threaded through to
  // positions_data_add_move via RecorderArgs.play_cap.
  int position_play_cap;
  // AUTOPLAY_TYPE_LEAVE_GEN only: a hard cap on games played across the whole
  // run, independent of whether the generations' rack targets are ever
  // reached. Leavegen otherwise stops only on its targets -- there is no
  // per-generation game count anywhere in num_games_or_min_rack_targets,
  // which carries the targets themselves -- so without this a run is
  // unbounded, which is what 0 means and what the CLI leavegen command does.
  // A contribute leave_generation task, whose single generation's target
  // belongs to the server rather than to the task, sets this so the task
  // cannot run indefinitely when its own forced-rack subset never reaches
  // that target.
  uint64_t leavegen_max_games;
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
  bool use_play_chooser[2];
  // Total clock for each PlayChooser player. Zero means untimed.
  double time_control_seconds[2];
  int overtime_penalty_points;
  double overtime_period_seconds;
  // Templates copied into each game runner, which supplies the per-game
  // timer and seed before constructing its choosers.
  PlayChooserStrategy play_chooser_strategies[2];
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

int autoplay_overtime_penalty_points(double overtime_seconds,
                                     int points_per_period,
                                     double period_seconds);

#endif
