#ifndef SIM_ARGS_H
#define SIM_ARGS_H

#include "../def/bai_defs.h"
#include "../ent/game.h"
#include "../ent/game_history.h"
#include "../ent/inference_results.h"
#include "../ent/rack.h"
#include "../ent/sim_results.h"
#include "../ent/thread_control.h"
#include "bai_logger.h"
#include "inference.h"
#include <stdint.h>

typedef struct SimArgs {
  int num_plies;
  const Game *game;
  const MoveList *move_list;
  Rack *known_opp_rack;
  WinPct *win_pcts;
  bool use_inference;
  bool use_heat_map;
  InferenceResults *inference_results;
  InferenceArgs inference_args;
  int num_threads;
  int print_interval;
  int max_num_display_plays;
  uint64_t seed;
  ThreadControl *thread_control;
  BAIOptions bai_options;
} SimArgs;

static inline void sim_args_fill(
    const int num_plays, const int num_plies, const MoveList *move_list,
    Rack *known_opp_rack, WinPct *win_pcts, InferenceResults *inference_results,
    ThreadControl *thread_control, const Game *game,
    const bool sim_with_inference, const bool use_heat_map,
    const int num_threads, const int print_interval,
    const int max_num_display_plays, const uint64_t seed,
    const uint64_t max_iterations, const uint64_t min_play_iterations,
    const double scond, const bai_threshold_t threshold,
    const int time_limit_seconds, const bai_sampling_rule_t sampling_rule,
    InferenceArgs *inference_args, SimArgs *sim_args) {
  sim_args->num_plies = num_plies;
  sim_args->move_list = move_list;
  sim_args->known_opp_rack = known_opp_rack;
  sim_args->win_pcts = win_pcts;
  sim_args->inference_results = inference_results;
  sim_args->thread_control = thread_control;
  sim_args->game = game;
  sim_args->use_inference = sim_with_inference;
  sim_args->use_heat_map = use_heat_map;
  sim_args->num_threads = num_threads;
  sim_args->print_interval = print_interval;
  sim_args->max_num_display_plays = max_num_display_plays;
  sim_args->seed = seed;
  if (sim_args->use_inference) {
    sim_args->inference_args = *inference_args;
  }
  sim_args->bai_options.sample_limit = max_iterations;
  sim_args->bai_options.sample_minimum = min_play_iterations;
  if (scond > 100 || threshold == BAI_THRESHOLD_NONE) {
    sim_args->bai_options.threshold = BAI_THRESHOLD_NONE;
  } else {
    sim_args->bai_options.delta = 1.0 - (scond / 100.0);
    sim_args->bai_options.threshold = threshold;
  }
  sim_args->bai_options.time_limit_seconds = time_limit_seconds;
  sim_args->bai_options.sampling_rule = sampling_rule;
  sim_args->bai_options.num_threads = num_threads;
}

#endif