#ifndef SIM_ARGS_H
#define SIM_ARGS_H

#include "../def/bai_defs.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/game_history.h"
#include "../ent/inference_args.h"
#include "../ent/inference_results.h"
#include "../ent/rack.h"
#include "../ent/sim_results.h"
#include "../ent/thread_control.h"
#include <math.h>
#include <stdint.h>

typedef struct SimArgs {
  int num_plies;
  const Game *game;
  const MoveList *move_list;
  int num_plays;
  Rack *known_opp_rack;
  WinPct *win_pcts;
  bool use_inference;
  bool use_heat_map;
  InferenceResults *inference_results;
  InferenceArgs inference_args;
  int num_threads;
  int print_interval;
  int max_num_display_plays;
  int max_num_display_plies;
  uint64_t seed;
  ThreadControl *thread_control;
  BAIOptions bai_options;
  // Utility weights for blending win% and spread into the BAI sample value.
  // Defaults: (1.0, 0.0, 100.0) -> pure win%, backward compatible. With
  // non-zero utility_w_spread, rv_sim_sample returns
  //   (w_winpct*wpct + w_spread*spread_sigmoid) / (w_winpct + w_spread)
  // where
  //   spread_sigmoid = 1 / (1 + exp(-spread_pts / utility_spread_scale))
  // a logistic sigmoid, strictly in (0, 1) for any finite spread.
  // utility_spread_scale is the sigmoid's scale parameter: the slope at
  // spread=0 is 1/(4*utility_spread_scale), and at spread = +/-scale the
  // sigmoid is ~0.731 / ~0.269.
  double utility_w_winpct;
  double utility_w_spread;
  double utility_spread_scale;
} SimArgs;

static inline void
sim_args_fill(const int num_plies, const MoveList *move_list,
              const int num_plays, Rack *known_opp_rack, WinPct *win_pcts,
              InferenceResults *inference_results,
              ThreadControl *thread_control, const Game *game,
              const bool sim_with_inference, const bool use_heat_map,
              const int num_threads, const int print_interval,
              const int max_num_display_plays, const int max_num_display_plies,
              const uint64_t seed, const uint64_t max_iterations,
              const uint64_t min_play_iterations, const double scond,
              const bai_threshold_t threshold, const int time_limit_seconds,
              const bai_sampling_rule_t sampling_rule, const double cutoff,
              const InferenceArgs *inference_args, SimArgs *sim_args) {
  sim_args->num_plies = num_plies;
  sim_args->move_list = move_list;
  sim_args->num_plays = num_plays;
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
  sim_args->max_num_display_plies = max_num_display_plies;
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
  sim_args->bai_options.cutoff = cutoff;
  // This will be overwritten in autoplay
  sim_args->bai_options.parent_worker_thread_index = 0;
  sim_args->bai_options.arm_avoid_prune = NULL;
  sim_args->bai_options.num_arm_avoid_prune = 0;
  // Default utility weights: pure win%, no spread contribution.
  sim_args->utility_w_winpct = 1.0;
  sim_args->utility_w_spread = 0.0;
  sim_args->utility_spread_scale = 100.0;
}

// Blend rollout win% and (sigmoid-normalized) spread into a single BAI
// sample value.
//
//   spread_sigmoid = 1 / (1 + exp(-spread_pts / spread_scale))
//   utility  = (w_winpct * wpct + w_spread * spread_sigmoid) / (w_winpct +
//   w_spread)
//
// When w_spread == 0 the function returns wpct exactly (no FP arithmetic),
// so the default configuration is byte-identical to the pre-change behavior.
// Both wpct and spread_sigmoid are in [0, 1] (spread_sigmoid in (0, 1)
// strictly), so the blend stays in [0, 1] -- BAI's sub-Gaussian threshold
// assumptions remain valid regardless of weight magnitudes.
//
// Unlike a clamp, the sigmoid preserves a (vanishing) distinction at extreme
// spreads: avg=+1000 with scale=100 gives spread_sigmoid ~= 1 - 4.5e-5, vs
// avg=+200 -> ~0.881, vs avg=+50 -> ~0.622. The slope at zero is
// 1/(4*spread_scale), so spread_scale = 100 means a 1-point spread change
// near zero shifts utility by ~0.0025 per unit weight.
static inline double sim_utility_blend(double wpct, Equity spread,
                                       double w_winpct, double w_spread,
                                       double spread_scale) {
  if (w_spread == 0.0) {
    return wpct;
  }
  const double s = equity_to_double(spread) / spread_scale;
  const double spread_sigmoid = 1.0 / (1.0 + exp(-s));
  return (w_winpct * wpct + w_spread * spread_sigmoid) / (w_winpct + w_spread);
}

#endif