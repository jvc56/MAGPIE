#ifndef SIM_ARGS_H
#define SIM_ARGS_H

#include "../def/bai_defs.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/game_history.h"
#include "../ent/inference_args.h"
#include "../ent/inference_results.h"
#include "../ent/leave_odds.h"
#include "../ent/rack.h"
#include "../ent/sim_results.h"
#include "../ent/thread_control.h"
#include "../ent/value_net_history.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

// Evaluates rows value net input rows (board then scalars, row-major) into
// values and, unless spreads is NULL, the spread head's outputs; context is
// the evaluator. Must be safe to call from every sim thread at once.
typedef void (*value_net_rows_fn)(void *context, int rows, const float *board,
                                  const float *scalars, float *values,
                                  float *spreads);

// The opponent-leave head (value_net.h's value_net_rack_head) for rows
// input rows with VALUE_NET_RACK_SIDE side inputs each: theta receives
// VALUE_NET_RACK_LETTERS log odds per row; context is the evaluator.
typedef void (*value_net_rack_odds_fn)(void *context, int rows,
                                       const float *board, const float *scalars,
                                       const float *side, float *theta);

typedef struct SimArgs {
  int num_plies;
  const Game *game;
  const MoveList *move_list;
  int num_plays;
  Rack *known_opp_rack;
  WinPct *win_pcts;
  bool use_inference;
  // With use_inference, each iteration draws the opponent's rack uniformly
  // from the unseen tiles instead of from the inferred leaves with this
  // probability, so a leave the inference ruled out is still possible.
  double inference_uniform_mix;
  // When set, a learned distribution over the leave the opponent kept from
  // their last move: an iteration draws their rack as a leave from it plus
  // tiles from the bag. With inferred leaves too (use_inference), it supplies
  // opponent_leave_odds_share of the draws that are not uniform. Owned by
  // the caller; inference_uniform_mix applies to it as well.
  const LeaveOdds *opponent_leave_odds;
  double opponent_leave_odds_share;
  // With use_inference: inference_results already holds the inferred leaves
  // (their alias method's tables generated), so simulate does not infer.
  bool inference_precomputed;
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
  // When true and the provided SimResults already holds simmed plays
  // matching the move list (same play count and ply count), the sim
  // skips the results reset and keeps accumulating samples onto the
  // existing per-play stats — resuming a previously stopped (or
  // saved-and-restored) simulation instead of starting from zero.
  // The move list must contain the same plays the SimResults was
  // built from; sampling reads moves from the SimmedPlays themselves.
  bool resume_results;
  // Utility weights for the BAI sample blend. Defaults (1.0, 0.0, 100.0)
  // are pure win%, backward compatible. See sim_utility_blend below for
  // the formula and the role of utility_spread_scale.
  double utility_w_winpct;
  double utility_w_spread;
  double utility_spread_scale;
  // The simming player's rollout PAT settings (see the patrollout and
  // patrolloutclasses options): whether every rollout ply leaves PAT out,
  // and which classes it suppresses otherwise. They apply to both players'
  // plies in the rollouts, each with its own weights; the candidates the sim
  // is handed come from the candidate-selection settings instead.
  // sim_args_fill clears them (PAT on, every class); callers set them after.
  bool pat_rollout_disabled;
  uint32_t pat_rollout_disabled_classes_mask;
  // Whether a nonterminal sim horizon's spread is projected to the end of the
  // game with the win percentage table's expected swing for that state (see
  // rv_sim_sample).
  bool use_margin_forecast;
  // When rollout_value_net_evaluate is set, the first
  // rollout_value_net_plies rollout plies (0..num_plies) are chosen by the
  // value net: the top rollout_value_net_candidates static plays (0 for 15),
  // the one with the highest utility by the weights above played
  // (value_net_utility in value_net_features.h); later plies play the top
  // static move. With rollout_value_net_leaf, the net also scores the final
  // ply's move (its own choice, or the static one), and an iteration whose
  // rollout did not end the game takes the net's win% and predicted final
  // spread for it instead of the win% table; so with 0 policy plies the
  // rollouts are static and only the leaf is the net's. Iterations are
  // computed rollout_value_net_batch at a time per thread and play (0 for 8),
  // advanced together with one call per ply that needs the net, which
  // evaluates rows input rows (value_net_defs.h) into values, and spreads
  // when the utility weighs spread. Before the candidate, the replier's
  // history is rollout_value_net_history and the simming player's
  // rollout_value_net_own_history. sim_args_fill clears the evaluator.
  value_net_rows_fn rollout_value_net_evaluate;
  void *rollout_value_net_context;
  int rollout_value_net_candidates;
  int rollout_value_net_batch;
  int rollout_value_net_plies;
  bool rollout_value_net_leaf;
  ValueNetHistory rollout_value_net_history;
  ValueNetHistory rollout_value_net_own_history;
} SimArgs;

// Unlike endgame_args_fill and peg_args_fill, this does NOT take a parameter
// for every SimArgs field: bai_options.arm_avoid_prune and
// parent_worker_thread_index are defaulted here and overwritten by the callers
// that care. Adding a SimArgs field therefore does not break the call sites the
// way it does for those two, so audit them by hand until this follows suit.
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
              const bai_threshold_t threshold, const double time_limit_seconds,
              const bai_sampling_rule_t sampling_rule, const double cutoff,
              const double utility_w_winpct, const double utility_w_spread,
              const double utility_spread_scale, const bool use_margin_forecast,
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
  sim_args->inference_uniform_mix = 0.0;
  sim_args->opponent_leave_odds = NULL;
  sim_args->opponent_leave_odds_share = 0.0;
  sim_args->inference_precomputed = false;
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
    // Unread while the threshold is NONE (only GK16 divides by it), but set
    // so no field is left to whatever the caller's storage happened to hold.
    sim_args->bai_options.delta = 1.0;
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
  // Pure win% (no spread contribution) is (1.0, 0.0, 100.0).
  sim_args->utility_w_winpct = utility_w_winpct;
  sim_args->utility_w_spread = utility_w_spread;
  sim_args->utility_spread_scale = utility_spread_scale;
  sim_args->use_margin_forecast = use_margin_forecast;
  sim_args->rollout_value_net_evaluate = NULL;
  sim_args->rollout_value_net_context = NULL;
  sim_args->rollout_value_net_candidates = 0;
  sim_args->rollout_value_net_batch = 0;
  sim_args->rollout_value_net_plies = 0;
  sim_args->rollout_value_net_leaf = false;
  value_net_history_reset(&sim_args->rollout_value_net_history);
  value_net_history_reset(&sim_args->rollout_value_net_own_history);
  // Start fresh, not resuming a prior SimResults. Only the TUI's analysis-
  // resume path sets this true; every other caller fills SimArgs through
  // here, so leaving it uninitialized let stack garbage spuriously trigger
  // a resume — skipping sim_results_reset and accumulating samples, which
  // made multi-threaded sims non-reproducible vs single-threaded.
  sim_args->resume_results = false;
  sim_args->pat_rollout_disabled = false;
  sim_args->pat_rollout_disabled_classes_mask = 0;
}

// Blend rollout win% and (sigmoid-normalized) spread into a single BAI
// sample value:
//
//   spread_sigmoid = 1 / (1 + exp(-spread_pts / spread_scale))   in (0, 1)
//   utility = (w_winpct * wpct + w_spread * spread_sigmoid)
//             / (w_winpct + w_spread)                            in [0, 1]
//
// spread_scale is the logistic's scale parameter: slope at spread=0 is
// 1/(4*spread_scale), and at spread = +/-scale the sigmoid is ~0.731/~0.269.
// The blend stays bounded in [0, 1] so BAI's sub-Gaussian threshold
// assumptions remain valid regardless of weight magnitudes.
//
// When w_spread == 0 the function returns wpct exactly with no FP
// arithmetic, so the default configuration is bit-identical to the
// pre-change behavior.
static inline double sim_utility_blend(double wpct, Equity spread,
                                       double w_winpct, double w_spread,
                                       double spread_scale) {
  if (w_spread == 0.0) {
    return wpct;
  }
  // Sign-branched sigmoid so exp() always takes a non-positive argument
  // and can underflow harmlessly to 0 instead of overflowing to +inf.
  const double scaled_spread = equity_to_double(spread) / spread_scale;
  double spread_sigmoid;
  if (scaled_spread >= 0.0) {
    spread_sigmoid = 1.0 / (1.0 + exp(-scaled_spread));
  } else {
    const double exp_scaled_spread = exp(scaled_spread);
    spread_sigmoid = exp_scaled_spread / (1.0 + exp_scaled_spread);
  }
  // Normalize weights before multiplying so the individual products
  // can't underflow before the division would otherwise cancel them.
  const double total_weight = w_winpct + w_spread;
  const double norm_winpct = w_winpct / total_weight;
  const double norm_spread = w_spread / total_weight;
  return norm_winpct * wpct + norm_spread * spread_sigmoid;
}

#endif