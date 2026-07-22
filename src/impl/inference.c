#include "inference.h"

#include "../compat/cpthread.h"
#include "../def/bai_defs.h"
#include "../def/cpthread_defs.h"
#include "../def/equity_defs.h"
#include "../def/game_history_defs.h"
#include "../def/inference_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/move_defs.h"
#include "../def/rack_defs.h"
#include "../def/thread_control_defs.h"
#include "../ent/alias_method.h"
#include "../ent/bag.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/game_history.h"
#include "../ent/inference_args.h"
#include "../ent/inference_results.h"
#include "../ent/klv.h"
#include "../ent/leave_rack.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../ent/sim_args.h"
#include "../ent/sim_results.h"
#include "../ent/stats.h"
#include "../ent/thread_control.h"
#include "../ent/validated_move.h"
#include "../ent/win_pct.h"
#include "../str/inference_string.h"
#include "../str/rack_string.h"
#include "../util/io_util.h"
#include "../util/math_util.h"
#include "../util/string_util.h"
#include "gameplay.h"
#include "move_gen.h"
#include "sim_fwd.h"
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct Inference {
  // KLV used to evaluate leaves to determine
  // which moves are top equity. This should be
  // the KLV of the target.
  const KLV *klv;
  ThreadControl *thread_control;

  // The following fields are owned by this struct.

  int ld_size;
  int leave_list_capacity;
  // Target player index in the game
  int target_index;
  // Target player score
  Equity target_score;
  // Number of tiles exchanged by the target
  int target_number_of_tiles_exchanged;
  // Maximum equity loss the target can
  // lose while still being considered
  // the top move.
  Equity equity_margin;
  bool use_infer_cutoff_optimization;
  uint64_t current_rack_index;
  int num_threads;
  int print_interval;
  int movegen_index;
  uint64_t *shared_rack_index;
  cpthread_mutex_t *shared_rack_index_lock;
  cpthread_t cpthread_id;
  // Rack containing just the unknown leave, which is
  // the tiles on the target's rack unseen to
  // the observer making the inference.
  Rack *current_target_leave;
  // Rack containing just the exchange, which is
  // the tiles the target put back into the bag which
  // are unseen to the observer making the inference.
  Rack *current_target_exchanged;
  // Rack containing the leave and the other tiles
  // which the observer may know about (for example, due
  // to a lost challenge, coffee housing, or accidental flash).
  Rack *current_target_rack;
  // The bag represented by a rack for convenience
  Rack *bag_as_rack;
  // MoveList used by the inference to generate moves (capacity 1)
  MoveList *move_list;
  // Game used by the inference to generate moves
  Game *game;
  InferenceResults *results;

  // Win-probability mode fields (only used when mode == INFERENCE_MODE_WINPCT)
  inference_mode_t mode;
  const WinPct *win_pcts;
  double tau;
  int mini_sim_plies;
  // Top-K candidate moves to consider per leave hypothesis
  int mini_sim_max_plays;
  // Per-leaf mini-sim iteration budget: sample_limit = num_candidates *
  // mini_sim_iters (matching Macondo's simIters=200 semantics).
  uint64_t mini_sim_iters;
  // Wall-clock time limit for the MC outer loop (seconds). 0 = no hard limit
  // (run until user interrupt). Set from InferenceArgs::mc_time_limit_secs.
  int mc_time_limit_secs;
  // Absolute deadline for the MC outer loop. Set by infer_mc_manager just
  // before launching worker threads so all workers share the same deadline.
  struct timespec mc_deadline;
  // The tiles the target actually played (pointer into InferenceArgs; valid
  // for the lifetime of this inference call).
  const Rack *target_played_tiles;
  // Full target move from game history (NULL when only score+tiles are known).
  // When non-NULL, candidate_matches_target uses exact positional comparison.
  const Move *target_move;
  // Candidate move list for winpct mode (capacity = mini_sim_max_plays)
  MoveList *candidate_move_list;
  // Reusable sim context and results for per-leaf mini-sims (winpct mode only)
  SimResults *mini_sim_results;
  SimCtx *mini_sim_ctx;
  ErrorStack *mini_sim_error_stack;
} Inference;

struct InferenceCtx {
  Game *game;
  int num_workers;
  Inference **worker_inferences;
  uint64_t shared_rack_index;
  cpthread_mutex_t shared_rack_index_lock;
  Stat **leave_stats;
  Stat **exchanged_stats;
  Stat **rack_stats;
};

void inference_destroy(Inference *inference) {
  if (!inference) {
    return;
  }
  rack_destroy(inference->current_target_leave);
  rack_destroy(inference->current_target_exchanged);
  rack_destroy(inference->bag_as_rack);
  move_list_destroy(inference->move_list);
  move_list_destroy(inference->candidate_move_list);
  sim_results_destroy(inference->mini_sim_results);
  sim_ctx_destroy(inference->mini_sim_ctx);
  error_stack_destroy(inference->mini_sim_error_stack);
  inference_results_destroy(inference->results);
  game_destroy(inference->game);
  free(inference);
}

uint64_t get_number_of_draws_for_rack(const Rack *bag_as_rack,
                                      const Rack *rack) {
  uint64_t number_of_ways = 1;
  for (int i = 0; i < rack_get_dist_size(rack); i++) {
    if (rack_get_letter(rack, i) > 0) {
      number_of_ways *=
          choose(rack_get_letter(bag_as_rack, i) + rack_get_letter(rack, i),
                 rack_get_letter(rack, i));
    }
  }
  return number_of_ways;
}

// Counts distinct multisets of size k drawable from bag_as_rack.
// Mirrors Macondo's rangefinder countMultisets (enumerate.go).
// Used to decide whether exhaustive enumeration is cheaper than MC sampling.
static int count_multisets_impl(const Rack *bag, int min_idx, int remaining,
                                int ld_size) {
  if (remaining == 0) {
    return 1;
  }
  int count = 0;
  for (int tile_idx = min_idx; tile_idx < ld_size; tile_idx++) {
    const int avail = (int)rack_get_letter(bag, (MachineLetter)tile_idx);
    if (avail == 0) {
      continue;
    }
    const int max_copies = remaining < avail ? remaining : avail;
    for (int cnt = 1; cnt <= max_copies; cnt++) {
      count +=
          count_multisets_impl(bag, tile_idx + 1, remaining - cnt, ld_size);
    }
  }
  return count;
}

static int count_multisets(const Rack *bag, int k, int ld_size) {
  return count_multisets_impl(bag, 0, k, ld_size);
}

// Computes the softmax likelihood of target_move_idx over logit(WinPct)/tau.
// mini_sim_results must be populated before calling.
// Fixed-size arrays (size INFERENCE_WINPCT_MAX_CANDIDATE_PLAYS) replace VLAs.
static double compute_softmax_likelihood(const Inference *inference,
                                         int target_move_idx,
                                         int num_candidates) {
  if (num_candidates == 0 || target_move_idx < 0 ||
      target_move_idx >= num_candidates) {
    return 0.0;
  }
  double logit_vals[INFERENCE_WINPCT_MAX_CANDIDATE_PLAYS];
  for (int cand_idx = 0; cand_idx < num_candidates; cand_idx++) {
    const SimmedPlay *sp =
        sim_results_get_simmed_play(inference->mini_sim_results, cand_idx);
    double wp = stat_get_mean(simmed_play_get_win_pct_stat(sp));
    if (wp < INFERENCE_WINPCT_LOGIT_EPS) {
      wp = INFERENCE_WINPCT_LOGIT_EPS;
    } else if (wp > 1.0 - INFERENCE_WINPCT_LOGIT_EPS) {
      wp = 1.0 - INFERENCE_WINPCT_LOGIT_EPS;
    }
    logit_vals[cand_idx] = log(wp / (1.0 - wp)) / inference->tau;
  }
  double max_logit = logit_vals[0];
  for (int cand_idx = 1; cand_idx < num_candidates; cand_idx++) {
    if (logit_vals[cand_idx] > max_logit) {
      max_logit = logit_vals[cand_idx];
    }
  }
  double exp_vals[INFERENCE_WINPCT_MAX_CANDIDATE_PLAYS];
  double sum_exp = 0.0;
  for (int cand_idx = 0; cand_idx < num_candidates; cand_idx++) {
    exp_vals[cand_idx] = exp(logit_vals[cand_idx] - max_logit);
    sum_exp += exp_vals[cand_idx];
  }
  return (sum_exp > 0.0) ? exp_vals[target_move_idx] / sum_exp : 0.0;
}

void increment_subtotals_for_results(const Rack *rack,
                                     InferenceResults *results,
                                     inference_stat_t inference_stat_type,
                                     uint64_t number_of_draws_for_leave) {
  for (int i = 0; i < rack_get_dist_size(rack); i++) {
    if (rack_get_letter(rack, i) > 0) {
      inference_results_add_to_letter_subtotal(
          results, inference_stat_type, i, rack_get_letter(rack, i),
          INFERENCE_SUBTOTAL_DRAW, number_of_draws_for_leave);
      inference_results_add_to_letter_subtotal(results, inference_stat_type, i,
                                               rack_get_letter(rack, i),
                                               INFERENCE_SUBTOTAL_LEAVE, 1);
    }
  }
}

void record_valid_leave(const Rack *rack, InferenceResults *results,
                        inference_stat_t inference_stat_type,
                        double current_leave_value,
                        uint64_t number_of_draws_for_leave) {
  stat_push(inference_results_get_equity_values(results, inference_stat_type),
            current_leave_value, number_of_draws_for_leave);
  increment_subtotals_for_results(rack, results, inference_stat_type,
                                  number_of_draws_for_leave);
}

// Returns true if the candidate move exactly matches the target move.
// row, col, direction, and tile sequence must all match.
// Tiles include PLAYED_THROUGH_MARKER entries and blanked letters
// (e.g. BLANK_MACHINE_LETTER | 'A'), compared verbatim — no normalization
// needed since candidate and target were generated under the same board state.
// Returns false when target_move is NULL (winpct inference requires game
// history; the explicit-arg CLI path is only used for equity mode).
static bool candidate_matches_target(const Move *move,
                                     const Inference *inference) {
  if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE ||
      inference->target_move == NULL) {
    return false;
  }
  const Move *target = inference->target_move;
  if (move_get_row_start(move) != move_get_row_start(target) ||
      move_get_col_start(move) != move_get_col_start(target) ||
      move_get_dir(move) != move_get_dir(target) ||
      move_get_tiles_length(move) != move_get_tiles_length(target)) {
    return false;
  }
  for (int tile_idx = 0; tile_idx < move_get_tiles_length(move); tile_idx++) {
    if (move_get_tile(move, tile_idx) != move_get_tile(target, tile_idx)) {
      return false;
    }
  }
  return true;
}

// Forward declaration: defined after evaluate_leave_winpct to keep the
// enum and MC paths grouped together, but called from evaluate_leave_winpct.
static double run_mini_sim_and_get_likelihood(Inference *inference,
                                              int *out_target_move_idx);

// Evaluates the current leave hypothesis in winpct (Bayesian) mode.
// For each leave L: weight = prior(L) * likelihood(L), where prior is the
// multivariate hypergeometric probability and likelihood is the softmax of
// logit(WinPct) / tau at the target move's index among the top-K candidates.
// The mini-sim uses BAI (GK16 + TopTwo) with the target move protected from
// pruning, mirroring Macondo's rangefinder Stop99 behaviour.
static void evaluate_leave_winpct(Inference *inference) {
  const double prior = (double)get_number_of_draws_for_rack(
      inference->bag_as_rack, inference->current_target_leave);
  if (prior == 0.0) {
    return;
  }
  inference_results_add_iter(inference->results);

  Game *game = inference->game;
  const int nontarget_index = 1 - inference->target_index;
  const int ld_size = inference->ld_size;

  // Save nontarget's current rack (always nontarget_known at entry) so we can
  // restore it after the mini-sim.
  Rack nontarget_initial_rack;
  rack_set_dist_size_and_reset(&nontarget_initial_rack, ld_size);
  rack_copy(&nontarget_initial_rack,
            player_get_rack(game_get_player(game, nontarget_index)));

  // The game bag still contains the leave hypothesis tiles
  // (increment_letter_for_inference only removes them from bag_as_rack, not
  // from the real game bag).  Draw them out so the bag matches bag_as_rack
  // before any mini-sim play.
  Bag *bag = game_get_bag(game);
  const int draw_index = game_get_player_on_turn_draw_index(game);
  for (int ml = 0; ml < ld_size; ml++) {
    const int count = (int)rack_get_letter(inference->current_target_leave, ml);
    for (int j = 0; j < count; j++) {
      bag_draw_letter(bag, (MachineLetter)ml, draw_index);
    }
  }

  // Fill the nontarget's rack randomly from the now-synced bag.
  draw_to_full_rack(game, nontarget_index);

  // Generate candidates, run BAI mini-sim, compute softmax likelihood.
  // run_mini_sim_and_get_likelihood returns 0 when the target is not in top-K,
  // matching Macondo's "if bayesianWeight <= 0 { return nil }" skip.
  const double likelihood = run_mini_sim_and_get_likelihood(inference, NULL);

  // Restore nontarget rack and game bag to their pre-evaluation state.
  // After simulate() the game is at the pre-sim snapshot; return_rack_to_bag
  // puts the nontarget tiles (nontarget_initial_rack + random draw) back,
  // then we re-draw just nontarget_initial_rack, then return the leave tiles.
  return_rack_to_bag(game, nontarget_index);
  if (!draw_rack_from_bag(game, nontarget_index, &nontarget_initial_rack)) {
    log_fatal("winpct inference: failed to restore nontarget rack");
  }
  for (int ml = 0; ml < ld_size; ml++) {
    const int count = (int)rack_get_letter(inference->current_target_leave, ml);
    for (int j = 0; j < count; j++) {
      bag_add_letter(bag, (MachineLetter)ml, draw_index);
    }
  }

  if (likelihood > 0.0) {
    const double weight = prior * likelihood;
    inference_results_accumulate_winpct_weight(
        inference->results, INFERENCE_TYPE_LEAVE,
        inference->current_target_leave, weight);
  }
}

// Helper: generate candidates + run mini-sim for the current game state.
// Returns the softmax likelihood of target_move_idx (>= 0), or 0.0 if the
// target is not found among candidates.  mini_sim_results is populated on
// return.  The game must already have target and nontarget racks set.
static double run_mini_sim_and_get_likelihood(Inference *inference,
                                              int *out_target_move_idx) {
  move_list_reset(inference->candidate_move_list);
  const MoveGenArgs gen_args = {
      .game = inference->game,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      .thread_index = inference->movegen_index,
      .move_list = inference->candidate_move_list,
      .tiles_played_bv = NULL,
      .initial_tiles_bv = 0,
  };
  generate_moves(&gen_args);
  int num_candidates = move_list_get_count(inference->candidate_move_list);

  int target_move_idx = -1;
  for (int cand_idx = 0; cand_idx < num_candidates; cand_idx++) {
    if (candidate_matches_target(
            move_list_get_move(inference->candidate_move_list, cand_idx),
            inference)) {
      target_move_idx = cand_idx;
      break;
    }
  }
  if (target_move_idx < 0 && inference->target_move != NULL) {
    // Target move not in top-N by equity: append it so it is always simmed.
    // Macondo (simplesimmer.go:99-112) does the same — the whole point of
    // winpct inference is that equity rank doesn't determine the posterior.
    // move_list_load_with_empty_moves allocates capacity + 1 slots, so
    // moves[num_candidates] is always a valid write when num_candidates <=
    // capacity. We also update count so the simulator allocates the right
    // number of SimmedPlay entries.
    move_copy(
        move_list_get_move(inference->candidate_move_list, num_candidates),
        inference->target_move);
    target_move_idx = num_candidates;
    num_candidates++;
    inference->candidate_move_list->count = num_candidates;
  }
  if (out_target_move_idx) {
    *out_target_move_idx = target_move_idx;
  }
  if (target_move_idx < 0 || num_candidates == 0) {
    return 0.0;
  }

  SimArgs mini_sim_args;
  const uint64_t sample_limit =
      (uint64_t)num_candidates * inference->mini_sim_iters;
  // Use INFERENCE_WINPCT_BAI_INITIAL_ITERS as sample_minimum to match Macondo's
  // minIterationsForPruning=128: every arm gets at least this many uniform
  // samples before GK16+TopTwo can prune/focus. Cap at mini_sim_iters so that
  // initial_limit never exceeds sample_limit.
  const uint64_t sample_minimum =
      inference->mini_sim_iters < INFERENCE_WINPCT_BAI_INITIAL_ITERS
          ? (uint64_t)inference->mini_sim_iters
          : INFERENCE_WINPCT_BAI_INITIAL_ITERS;
  sim_args_fill(inference->mini_sim_plies, inference->candidate_move_list,
                num_candidates, NULL, (WinPct *)inference->win_pcts, NULL,
                inference->thread_control, inference->game, false, false, 1, 0,
                0, 0, 0, sample_limit, sample_minimum, 99.0,
                BAI_THRESHOLD_GK16, 0,
                BAI_SAMPLING_RULE_TOP_TWO_IDS, 0.0, NULL, &mini_sim_args);
  mini_sim_args.bai_options.parent_worker_thread_index =
      inference->movegen_index;
  int target_arm = target_move_idx;
  mini_sim_args.bai_options.arm_avoid_prune = &target_arm;
  mini_sim_args.bai_options.num_arm_avoid_prune = 1;

  error_stack_reset(inference->mini_sim_error_stack);
  inference_results_add_sim(inference->results);
  simulate(&mini_sim_args, &inference->mini_sim_ctx,
           inference->mini_sim_results, inference->mini_sim_error_stack);

  if (!error_stack_is_empty(inference->mini_sim_error_stack)) {
    return 0.0;
  }
  return compute_softmax_likelihood(inference, target_move_idx, num_candidates);
}

// MC path for tile placement: draw a random leave, run mini-sim, accumulate
// weight = likelihood only (no prior — importance sampling already draws from
// the hypergeometric prior via set_random_rack).
// Mirrors Macondo rangefinder inferSingle (inference.go:570).
// Entry: target rack = target_played ∪ target_known, nontarget =
// nontarget_known. Exit: game state restored to entry state.
static void mc_evaluate_tile_placement(Inference *inference,
                                       const Rack *target_initial_rack,
                                       const Rack *nontarget_initial_rack) {
  inference_results_add_iter(inference->results);
  Game *game = inference->game;
  const int ld_size = inference->ld_size;
  const int target_index = inference->target_index;
  const int nontarget_index = 1 - target_index;

  // Return known tiles to bag, draw them back, then fill the rest randomly.
  // The extras drawn are the leave hypothesis.
  set_random_rack(game, target_index, target_initial_rack);

  // Extract leave = full_target_rack - target_initial_rack
  Rack mc_leave;
  rack_set_dist_size_and_reset(&mc_leave, ld_size);
  rack_copy(&mc_leave, player_get_rack(game_get_player(game, target_index)));
  rack_subtract_using_floor_zero(&mc_leave, target_initial_rack);

  // Fill nontarget randomly from the remaining bag.
  draw_to_full_rack(game, nontarget_index);

  // Run mini-sim and compute likelihood.
  const double likelihood = run_mini_sim_and_get_likelihood(inference, NULL);

  // Restore game state.
  return_rack_to_bag(game, nontarget_index);
  if (!draw_rack_from_bag(game, nontarget_index, nontarget_initial_rack)) {
    log_fatal("mc winpct inference: failed to restore nontarget rack");
  }
  return_rack_to_bag(game, target_index);
  if (!draw_rack_from_bag(game, target_index, target_initial_rack)) {
    log_fatal("mc winpct inference: failed to restore target rack");
  }

  if (likelihood > 0.0) {
    inference_results_accumulate_winpct_weight(
        inference->results, INFERENCE_TYPE_LEAVE, &mc_leave, likelihood);
  }
}

// MC path for exchange: draw a random full rack, check for candidate exchanges,
// run mini-sim, accumulate per-candidate likelihoods.
// Mirrors Macondo rangefinder inferSingleExchange (inference.go:640).
// Entry: target rack = target_known (typically empty), nontarget =
// nontarget_known. Exit: game state restored to entry state.
static void mc_evaluate_exchange(Inference *inference,
                                 const Rack *target_initial_rack,
                                 const Rack *nontarget_initial_rack) {
  inference_results_add_iter(inference->results);
  Game *game = inference->game;
  const int ld_size = inference->ld_size;
  const int target_index = inference->target_index;
  const int nontarget_index = 1 - target_index;
  const int target_num_exch = inference->target_number_of_tiles_exchanged;

  // Draw a completely random full rack for the target (equivalent to
  // Macondo's SetRandomRack(opp, nil) in inferSingleExchange).
  return_rack_to_bag(game, target_index);
  draw_to_full_rack(game, target_index);

  // Generate top-15 candidate moves (Macondo numMoves=15 for exchange).
  move_list_reset(inference->candidate_move_list);
  const MoveGenArgs gen_args = {
      .game = game,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      .thread_index = inference->movegen_index,
      .move_list = inference->candidate_move_list,
      .tiles_played_bv = NULL,
      .initial_tiles_bv = 0,
  };
  generate_moves(&gen_args);
  const int num_candidates =
      move_list_get_count(inference->candidate_move_list);

  // Check if any exchange of matching count exists (Macondo early-exit).
  int exchange_count = 0;
  for (int cand_idx = 0; cand_idx < num_candidates; cand_idx++) {
    const Move *cand =
        move_list_get_move(inference->candidate_move_list, cand_idx);
    if (move_get_type(cand) == GAME_EVENT_EXCHANGE &&
        move_get_tiles_played(cand) == target_num_exch) {
      exchange_count++;
    }
  }
  if (exchange_count == 0) {
    return_rack_to_bag(game, target_index);
    if (!draw_rack_from_bag(game, target_index, target_initial_rack)) {
      log_fatal(
          "mc exchange inference: failed to restore target rack after skip");
    }
    return;
  }

  // Fill nontarget randomly for the mini-sim.
  draw_to_full_rack(game, nontarget_index);

  // Run mini-sim (no specific arm protected — all exchange arms compete).
  SimArgs mini_sim_args;
  const uint64_t sample_limit =
      (uint64_t)num_candidates * inference->mini_sim_iters;
  sim_args_fill(inference->mini_sim_plies, inference->candidate_move_list,
                num_candidates, NULL, (WinPct *)inference->win_pcts, NULL,
                inference->thread_control, game, false, false, 1, 0, 0, 0, 0,
                sample_limit, 1, 99.0, BAI_THRESHOLD_GK16, 0,
                BAI_SAMPLING_RULE_TOP_TWO_IDS, 0.0, NULL, &mini_sim_args);
  mini_sim_args.bai_options.parent_worker_thread_index =
      inference->movegen_index;

  error_stack_reset(inference->mini_sim_error_stack);
  simulate(&mini_sim_args, &inference->mini_sim_ctx,
           inference->mini_sim_results, inference->mini_sim_error_stack);

  // For each matching exchange, compute its softmax likelihood and accumulate.
  if (error_stack_is_empty(inference->mini_sim_error_stack)) {
    const Rack *full_target_rack =
        player_get_rack(game_get_player(game, target_index));
    for (int cand_idx = 0; cand_idx < num_candidates; cand_idx++) {
      const Move *cand =
          move_list_get_move(inference->candidate_move_list, cand_idx);
      if (move_get_type(cand) != GAME_EVENT_EXCHANGE ||
          move_get_tiles_played(cand) != target_num_exch) {
        continue;
      }
      const double likelihood =
          compute_softmax_likelihood(inference, cand_idx, num_candidates);
      if (likelihood <= 0.0) {
        continue;
      }
      // Leave = full rack minus the exchanged tiles in this candidate.
      Rack exchange_leave;
      rack_set_dist_size_and_reset(&exchange_leave, ld_size);
      rack_copy(&exchange_leave, full_target_rack);
      for (int tile_idx = 0; tile_idx < move_get_tiles_played(cand);
           tile_idx++) {
        rack_take_letter(&exchange_leave, move_get_tile(cand, tile_idx));
      }
      inference_results_accumulate_winpct_weight(inference->results,
                                                 INFERENCE_TYPE_LEAVE,
                                                 &exchange_leave, likelihood);
    }
  }

  // Restore game state.
  return_rack_to_bag(game, nontarget_index);
  if (!draw_rack_from_bag(game, nontarget_index, nontarget_initial_rack)) {
    log_fatal("mc exchange inference: failed to restore nontarget rack");
  }
  return_rack_to_bag(game, target_index);
  if (!draw_rack_from_bag(game, target_index, target_initial_rack)) {
    log_fatal("mc exchange inference: failed to restore target rack");
  }
}

// MC worker: runs until thread_control reports user interrupt or the wall-clock
// deadline set by infer_mc_manager is reached (mc_time_limit_secs seconds from
// launch). Mirrors Macondo's rangefinder goroutines which loop until
// context.Done() (default 1-minute deadline). Weight = likelihood only (no
// prior — importance sampling via set_random_rack).
static void *mc_worker(void *uncasted_inference) {
  Inference *inference = (Inference *)uncasted_inference;
  const Game *game = inference->game;
  const int target_index = inference->target_index;
  const int nontarget_index = 1 - target_index;
  const int ld_size = inference->ld_size;
  const bool is_exchange = inference->target_number_of_tiles_exchanged > 0;
  const bool has_deadline = inference->mc_time_limit_secs > 0;

  // Snapshot initial rack states established by complete_inference_setup.
  Rack target_initial_rack;
  rack_set_dist_size_and_reset(&target_initial_rack, ld_size);
  rack_copy(&target_initial_rack,
            player_get_rack(game_get_player(game, target_index)));

  Rack nontarget_initial_rack;
  rack_set_dist_size_and_reset(&nontarget_initial_rack, ld_size);
  rack_copy(&nontarget_initial_rack,
            player_get_rack(game_get_player(game, nontarget_index)));

  while (thread_control_get_status(inference->thread_control) !=
         THREAD_CONTROL_STATUS_USER_INTERRUPT) {
    if (has_deadline) {
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      if (now.tv_sec > inference->mc_deadline.tv_sec ||
          (now.tv_sec == inference->mc_deadline.tv_sec &&
           now.tv_nsec >= inference->mc_deadline.tv_nsec)) {
        break;
      }
    }

    if (is_exchange) {
      mc_evaluate_exchange(inference, &target_initial_rack,
                           &nontarget_initial_rack);
    } else {
      mc_evaluate_tile_placement(inference, &target_initial_rack,
                                 &nontarget_initial_rack);
    }
  }
  return NULL;
}

void evaluate_possible_leave(Inference *inference) {
  Equity current_leave_value =
      klv_get_leave_value(inference->klv, inference->current_target_leave);

  const Equity target_equity_cutoff =
      inference->target_score + current_leave_value + inference->equity_margin;
  int target_leave_size = UNSET_LEAVE_SIZE;
  Equity eq_margin_movegen = 0;
  if (inference->use_infer_cutoff_optimization &&
      (inference->target_number_of_tiles_exchanged > 0)) {
    target_leave_size = RACK_SIZE - inference->target_number_of_tiles_exchanged;
    // For exchanges, pass the margin to move generation for best_leaves
    // calculation
    eq_margin_movegen = inference->equity_margin;
  }

  // For tile placements, margin is already in target_equity_cutoff, so pass 0
  const Move *top_move = get_top_equity_move_for_inferences(
      inference->game, inference->movegen_index, inference->move_list,
      inference->use_infer_cutoff_optimization ? target_equity_cutoff
                                               : EQUITY_MAX_VALUE,
      target_leave_size, eq_margin_movegen);

  const bool is_within_equity_margin =
      target_equity_cutoff >= move_get_equity(top_move);
  const int tiles_played = move_get_tiles_played(top_move);
  const bool number_exchanged_matches =
      move_get_type(top_move) == GAME_EVENT_EXCHANGE &&
      tiles_played == inference->target_number_of_tiles_exchanged;
  const bool recordable = is_within_equity_margin || number_exchanged_matches ||
                          rack_is_empty(inference->bag_as_rack);

  if (recordable) {
    uint64_t number_of_draws_for_leave = get_number_of_draws_for_rack(
        inference->bag_as_rack, inference->current_target_leave);
    if (inference->target_number_of_tiles_exchanged > 0) {
      record_valid_leave(inference->current_target_leave, inference->results,
                         INFERENCE_TYPE_RACK,
                         equity_to_double(current_leave_value),
                         number_of_draws_for_leave);

      if (number_exchanged_matches) {
        // The full rack for the exchange was recorded above,
        // but now we have to record the leave and the exchanged tiles
        for (int exchanged_tile_index = 0; exchanged_tile_index < tiles_played;
             exchanged_tile_index++) {
          MachineLetter tile_exchanged =
              move_get_tile(top_move, exchanged_tile_index);
          rack_add_letter(inference->current_target_exchanged, tile_exchanged);
          rack_take_letter(inference->current_target_leave, tile_exchanged);
        }
        record_valid_leave(
            inference->current_target_leave, inference->results,
            INFERENCE_TYPE_LEAVE,
            equity_to_double(klv_get_leave_value(
                inference->klv, inference->current_target_leave)),
            number_of_draws_for_leave);
        record_valid_leave(
            inference->current_target_exchanged, inference->results,
            INFERENCE_TYPE_EXCHANGED,
            equity_to_double(klv_get_leave_value(
                inference->klv, inference->current_target_exchanged)),
            number_of_draws_for_leave);
        LeaveRackList *lrl =
            inference_results_get_leave_rack_list(inference->results);
        if (lrl) {
          leave_rack_list_insert_rack(inference->current_target_leave,
                                      inference->current_target_exchanged,
                                      (int)number_of_draws_for_leave,
                                      current_leave_value, lrl);
        }
        alias_method_add_rack(
            inference_results_get_alias_method(inference->results),
            inference->current_target_leave, (int)number_of_draws_for_leave);
        rack_reset(inference->current_target_exchanged);
        for (int exchanged_tile_index = 0; exchanged_tile_index < tiles_played;
             exchanged_tile_index++) {
          MachineLetter tile_exchanged =
              move_get_tile(top_move, exchanged_tile_index);
          rack_add_letter(inference->current_target_leave, tile_exchanged);
        }
      }
    } else {
      record_valid_leave(inference->current_target_leave, inference->results,
                         INFERENCE_TYPE_LEAVE,
                         equity_to_double(current_leave_value),
                         number_of_draws_for_leave);
      alias_method_add_rack(
          inference_results_get_alias_method(inference->results),
          inference->current_target_leave, (int)number_of_draws_for_leave);
      LeaveRackList *lrl =
          inference_results_get_leave_rack_list(inference->results);
      if (lrl) {
        leave_rack_list_insert_rack(inference->current_target_leave, NULL,
                                    (int)number_of_draws_for_leave,
                                    current_leave_value, lrl);
      }
    }
  }
}

void increment_letter_for_inference(Inference *inference,
                                    MachineLetter letter) {
  rack_take_letter(inference->bag_as_rack, letter);
  rack_add_letter(inference->current_target_rack, letter);
  rack_add_letter(inference->current_target_leave, letter);
}

void decrement_letter_for_inference(Inference *inference,
                                    MachineLetter letter) {
  rack_add_letter(inference->bag_as_rack, letter);
  rack_take_letter(inference->current_target_rack, letter);
  rack_take_letter(inference->current_target_leave, letter);
}

void complete_inference_setup(Inference *inference, const InferenceArgs *args) {
  // This will return the inference->current_target_rack to the bag.
  return_rack_to_bag(inference->game, 0);
  return_rack_to_bag(inference->game, 1);

  Rack temp_target_rack;
  rack_set_dist_size_and_reset(&temp_target_rack, inference->ld_size);

  rack_union(&temp_target_rack, args->target_played_tiles);
  rack_union(&temp_target_rack, args->target_known_rack);

  bool success = draw_rack_from_bag(inference->game, args->target_index,
                                    &temp_target_rack);
  if (!success) {
    const LetterDistribution *ld = game_get_ld(inference->game);
    StringBuilder *sb = string_builder_create();
    string_builder_add_string(sb, "failed to draw combined (");
    string_builder_add_rack(sb, &temp_target_rack, ld, false);
    string_builder_add_string(sb, ") inferred player played letters (");
    string_builder_add_rack(sb, args->target_played_tiles, ld, false);
    string_builder_add_string(sb, ") and inferred player known rack (");
    string_builder_add_rack(sb, args->target_known_rack, ld, false);
    string_builder_add_string(sb, ") from the bag");
    char *err_msg = string_builder_dump(sb, NULL);
    string_builder_destroy(sb);
    log_fatal(err_msg);
    free(err_msg);
  }

  success = draw_rack_from_bag(inference->game, 1 - args->target_index,
                               args->nontarget_known_rack);

  if (!success) {
    const LetterDistribution *ld = game_get_ld(inference->game);
    StringBuilder *sb = string_builder_create();
    string_builder_add_string(sb, "failed to draw nontarget player rack (");
    string_builder_add_rack(sb, args->nontarget_known_rack, ld, false);
    string_builder_add_string(sb, ") from the bag");
    char *err_msg = string_builder_dump(sb, NULL);
    string_builder_destroy(sb);
    log_fatal(err_msg);
    free(err_msg);
  }

  // Add the letters that are known to have been kept on the rack
  // for their target inferred play.
  rack_copy(inference->current_target_leave, args->target_known_rack);
  rack_subtract_using_floor_zero(inference->current_target_leave,
                                 args->target_played_tiles);

  // Set the bag_as_rack to the bag
  const Bag *bag = game_get_bag(inference->game);
  int bag_letters_array[MAX_ALPHABET_SIZE];
  memset(bag_letters_array, 0, sizeof(bag_letters_array));
  bag_increment_unseen_count(bag, bag_letters_array);
  for (int i = 0; i < MAX_ALPHABET_SIZE; i++) {
    rack_add_letters(inference->bag_as_rack, i, bag_letters_array[i]);
  }
}

Inference *inference_create(const Game *game, int thread_index,
                            const InferenceArgs *args,
                            const InferenceResults *results) {
  Inference *inference = malloc_or_die(sizeof(Inference));
  inference->game = game_duplicate(game);
  inference->move_list = move_list_create(1);
  inference->klv =
      player_get_klv(game_get_player(inference->game, args->target_index));

  inference->ld_size = ld_get_size(game_get_ld(inference->game));
  inference->leave_list_capacity = args->leave_list_capacity;
  inference->target_index = args->target_index;
  inference->target_score = args->target_score;
  inference->target_number_of_tiles_exchanged = args->target_num_exch;
  inference->equity_margin = args->equity_margin;
  // Disable cutoff optimization for exchanges with large leaves
  // as benchmarks show it hurts performance for small exchanges
  const int leave_size = RACK_SIZE - args->target_num_exch;
  const bool is_small_exchange =
      args->target_num_exch > 0 &&
      leave_size >= INFERENCE_CUTOFF_MIN_EXCHANGE_LEAVE_SIZE;
  inference->use_infer_cutoff_optimization =
      args->use_inference_cutoff_optimization && !is_small_exchange;
  inference->current_rack_index = 0;

  inference->current_target_leave = rack_create(inference->ld_size);
  inference->current_target_exchanged = rack_create(inference->ld_size);
  inference->current_target_rack =
      player_get_rack(game_get_player(inference->game, args->target_index));
  inference->bag_as_rack = rack_create(inference->ld_size);

  inference->results =
      inference_results_create(inference_results_get_alias_method(results));
  inference_results_reset(inference->results, inference->leave_list_capacity,
                          inference->ld_size);

  // Multithreading
  inference->num_threads = args->num_threads;
  inference->print_interval = args->print_interval;
  inference->thread_control = args->thread_control;
  if (args->parent_worker_thread_index > 0 && thread_index > 0) {
    log_fatal("Both parent worker thread index (%d) and inference thread "
              "index (%d) are greater than 0.",
              args->parent_worker_thread_index, thread_index);
  }

  inference->movegen_index = args->parent_worker_thread_index + thread_index;

  // Win-probability mode fields.
  inference->mode = args->mode;
  inference->win_pcts = args->win_pcts;
  inference->tau = args->tau;
  inference->mini_sim_plies = args->mini_sim_plies;
  inference->mini_sim_max_plays = args->mini_sim_max_plays;
  inference->mini_sim_iters = args->mc_max_iters;
  inference->mc_time_limit_secs = args->mc_time_limit_secs;
  inference->target_played_tiles = args->target_played_tiles;
  inference->target_move = args->target_move;
  if (args->mode == INFERENCE_MODE_WINPCT) {
    // Capacity must cover both tile-placement (mini_sim_max_plays, default 10)
    // and exchange (INFERENCE_WINPCT_MAX_CANDIDATE_PLAYS = 15).
    const int list_capacity =
        args->mini_sim_max_plays > INFERENCE_WINPCT_MAX_CANDIDATE_PLAYS
            ? args->mini_sim_max_plays
            : INFERENCE_WINPCT_MAX_CANDIDATE_PLAYS;
    inference->candidate_move_list = move_list_create(list_capacity);
    inference->mini_sim_results = sim_results_create(0.0);
    inference->mini_sim_ctx = NULL;
    inference->mini_sim_error_stack = error_stack_create();
  } else {
    inference->candidate_move_list = NULL;
    inference->mini_sim_results = NULL;
    inference->mini_sim_ctx = NULL;
    inference->mini_sim_error_stack = NULL;
  }

  complete_inference_setup(inference, args);

  return inference;
}

void inference_reset(Inference *inference, const Game *game,
                     const InferenceArgs *args) {
  // Only update fields that will change between
  // inferences that happen in autoplay simming.
  game_copy(inference->game, game);

  inference->target_index = args->target_index;
  inference->target_score = args->target_score;
  inference->target_number_of_tiles_exchanged = args->target_num_exch;
  inference->equity_margin = args->equity_margin;
  // Disable cutoff optimization for exchanges with large leaves
  const int leave_size = RACK_SIZE - args->target_num_exch;
  const bool is_small_exchange =
      args->target_num_exch > 0 &&
      leave_size >= INFERENCE_CUTOFF_MIN_EXCHANGE_LEAVE_SIZE;
  inference->use_infer_cutoff_optimization =
      args->use_inference_cutoff_optimization && !is_small_exchange;
  inference->current_rack_index = 0;

  rack_reset(inference->current_target_leave);
  rack_reset(inference->current_target_exchanged);
  inference->current_target_rack = player_get_rack(
      game_get_player(inference->game, inference->target_index));
  rack_reset(inference->bag_as_rack);

  inference_results_reset(inference->results, inference->leave_list_capacity,
                          inference->ld_size);

  inference->mode = args->mode;
  inference->win_pcts = args->win_pcts;
  inference->tau = args->tau;
  inference->mini_sim_plies = args->mini_sim_plies;
  inference->mini_sim_max_plays = args->mini_sim_max_plays;
  inference->mini_sim_iters = args->mc_max_iters;
  inference->mc_time_limit_secs = args->mc_time_limit_secs;
  inference->target_played_tiles = args->target_played_tiles;
  inference->target_move = args->target_move;

  complete_inference_setup(inference, args);
}

void add_inference_results(InferenceResults *inference_results_to_add,
                           InferenceResults *inference_results_to_update) {
  inference_results_add_subtotals(inference_results_to_add,
                                  inference_results_to_update);
  LeaveRackList *lrl_to_add =
      inference_results_get_leave_rack_list(inference_results_to_add);
  if (!lrl_to_add) {
    // This means that lrl_to_update is also necessarily NULL, so no leave racks
    // to add.
    return;
  }
  LeaveRackList *lrl_to_update =
      inference_results_get_leave_rack_list(inference_results_to_update);

  while (leave_rack_list_get_count(lrl_to_add) > 0) {
    const LeaveRack *leave_rack_to_add = leave_rack_list_pop_rack(lrl_to_add);
    leave_rack_list_insert_leave_rack(leave_rack_to_add, lrl_to_update);
  }
}

bool should_print_info(const Inference *inference) {
  return inference->print_interval > 0 && inference->current_rack_index > 0 &&
         inference->current_rack_index % inference->print_interval == 0;
}

void iterate_through_all_possible_leaves(Inference *inference,
                                         int tiles_to_infer, int start_letter) {
  if (thread_control_get_status(inference->thread_control) ==
      THREAD_CONTROL_STATUS_USER_INTERRUPT) {
    return;
  }
  if (tiles_to_infer == 0) {
    bool perform_evaluation = false;
    bool print_info = false;

    cpthread_mutex_lock(inference->shared_rack_index_lock);
    if (inference->current_rack_index == *inference->shared_rack_index) {
      print_info = should_print_info(inference);
      perform_evaluation = true;
      *inference->shared_rack_index += 1;
    }
    cpthread_mutex_unlock(inference->shared_rack_index_lock);

    if (perform_evaluation) {
      if (inference->mode == INFERENCE_MODE_WINPCT) {
        evaluate_leave_winpct(inference);
      } else {
        evaluate_possible_leave(inference);
      }
    }
    if (print_info) {
      print_ucgi_inference_current_rack(inference->current_rack_index,
                                        inference->thread_control);
    }
    inference->current_rack_index++;
    return;
  }
  for (int letter = start_letter; letter < inference->ld_size; letter++) {
    if (rack_get_letter(inference->bag_as_rack, letter) > 0) {
      increment_letter_for_inference(inference, letter);
      iterate_through_all_possible_leaves(inference, tiles_to_infer - 1,
                                          letter);
      decrement_letter_for_inference(inference, letter);
    }
  }
}

void *infer_worker(void *uncasted_inference) {
  Inference *inference = (Inference *)uncasted_inference;
  iterate_through_all_possible_leaves(
      inference,
      (RACK_SIZE)-rack_get_total_letters(inference->current_target_rack),
      BLANK_MACHINE_LETTER);
  return NULL;
}

void set_shared_variables_for_inference(
    Inference *inference, uint64_t *shared_rack_index,
    cpthread_mutex_t *shared_rack_index_lock) {
  inference->shared_rack_index = shared_rack_index;
  inference->shared_rack_index_lock = shared_rack_index_lock;
}

// MC variant of infer_manager: launches mc_worker threads instead of
// infer_worker threads. MC workers run until the wall-clock deadline (set from
// mc_time_limit_secs) or a user interrupt, mirroring Macondo's goroutines
// running until context.Done().
static void infer_mc_manager(InferenceCtx *ctx, InferenceResults *results) {
  // Compute absolute deadline once and propagate to all workers so they share
  // the same cutoff regardless of thread scheduling jitter.
  const int time_limit_secs = ctx->worker_inferences[0]->mc_time_limit_secs;
  if (time_limit_secs > 0) {
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += time_limit_secs;
    for (int thread_index = 0; thread_index < ctx->num_workers;
         thread_index++) {
      ctx->worker_inferences[thread_index]->mc_deadline = deadline;
    }
  }

  for (int thread_index = 0; thread_index < ctx->num_workers; thread_index++) {
    cpthread_create(&ctx->worker_inferences[thread_index]->cpthread_id,
                    mc_worker, ctx->worker_inferences[thread_index]);
  }

  for (int thread_index = 0; thread_index < ctx->num_workers; thread_index++) {
    cpthread_join(ctx->worker_inferences[thread_index]->cpthread_id);
    InferenceResults *worker_results =
        ctx->worker_inferences[thread_index]->results;
    add_inference_results(worker_results, results);
    ctx->leave_stats[thread_index] = inference_results_get_equity_values(
        worker_results, INFERENCE_TYPE_LEAVE);
  }

  stats_combine(
      ctx->leave_stats, ctx->num_workers,
      inference_results_get_equity_values(results, INFERENCE_TYPE_LEAVE));
}

void infer_manager(InferenceCtx *ctx, InferenceResults *results) {
  for (int thread_index = 0; thread_index < ctx->num_workers; thread_index++) {
    cpthread_create(&ctx->worker_inferences[thread_index]->cpthread_id,
                    infer_worker, ctx->worker_inferences[thread_index]);
  }

  const bool tiles_were_exchanged =
      ctx->worker_inferences[0]->target_number_of_tiles_exchanged > 0;

  for (int thread_index = 0; thread_index < ctx->num_workers; thread_index++) {
    cpthread_join(ctx->worker_inferences[thread_index]->cpthread_id);
    InferenceResults *worker_results =
        ctx->worker_inferences[thread_index]->results;
    add_inference_results(worker_results, results);
    ctx->leave_stats[thread_index] = inference_results_get_equity_values(
        worker_results, INFERENCE_TYPE_LEAVE);
    if (tiles_were_exchanged) {
      ctx->exchanged_stats[thread_index] = inference_results_get_equity_values(
          worker_results, INFERENCE_TYPE_EXCHANGED);
      ctx->rack_stats[thread_index] = inference_results_get_equity_values(
          worker_results, INFERENCE_TYPE_RACK);
    }
  }

  stats_combine(
      ctx->leave_stats, ctx->num_workers,
      inference_results_get_equity_values(results, INFERENCE_TYPE_LEAVE));
  if (tiles_were_exchanged) {
    stats_combine(
        ctx->exchanged_stats, ctx->num_workers,
        inference_results_get_equity_values(results, INFERENCE_TYPE_EXCHANGED));
    stats_combine(
        ctx->rack_stats, ctx->num_workers,
        inference_results_get_equity_values(results, INFERENCE_TYPE_RACK));
  }
}

void verify_inference_args(const InferenceArgs *args, const Game *game_dup,
                           ErrorStack *error_stack) {
  const Bag *bag = game_get_bag(game_dup);
  int bag_letter_counts[MAX_ALPHABET_SIZE];
  memset(bag_letter_counts, 0, sizeof(bag_letter_counts));
  bag_increment_unseen_count(bag, bag_letter_counts);

  const int ld_size = ld_get_size(game_get_ld(game_dup));

  // Add the current player racks to the bag letter counts
  const Rack *player0_rack = player_get_rack(game_get_player(game_dup, 0));
  const Rack *player1_rack = player_get_rack(game_get_player(game_dup, 1));
  for (int i = 0; i < ld_size; i++) {
    bag_letter_counts[i] +=
        rack_get_letter(player0_rack, i) + rack_get_letter(player1_rack, i);
  }

  Rack temp_target_rack;
  rack_set_dist_size_and_reset(&temp_target_rack, ld_size);

  rack_union(&temp_target_rack, args->target_played_tiles);
  rack_union(&temp_target_rack, args->target_known_rack);

  if (rack_get_total_letters(&temp_target_rack) > (RACK_SIZE)) {
    const LetterDistribution *ld = game_get_ld(game_dup);
    StringBuilder *sb = string_builder_create();
    string_builder_add_string(sb, "inferred player rack (");
    string_builder_add_rack(sb, &temp_target_rack, ld, false);
    string_builder_add_string(sb,
                              ") derived from the union of played letters (");
    string_builder_add_rack(sb, args->target_played_tiles, ld, false);
    string_builder_add_string(sb, ") and known letters (");
    string_builder_add_rack(sb, args->target_known_rack, ld, false);
    string_builder_add_string(sb,
                              ") is greater than the maximum rack size of ");
    string_builder_add_int(sb, RACK_SIZE);
    error_stack_push(error_stack, ERROR_STATUS_INFERENCE_RACK_OVERFLOW,
                     string_builder_dump(sb, NULL));
    string_builder_destroy(sb);
    return;
  }

  for (int i = 0; i < ld_size; i++) {
    bag_letter_counts[i] -= rack_get_letter(&temp_target_rack, i);
    if (bag_letter_counts[i] < 0) {
      StringBuilder *sb = string_builder_create();
      string_builder_add_string(sb, "inferred player played letters (");
      string_builder_add_rack(sb, &temp_target_rack, game_get_ld(game_dup),
                              false);
      string_builder_add_string(sb, ") not available in the bag");
      error_stack_push(error_stack,
                       ERROR_STATUS_INFERENCE_TARGET_LETTERS_NOT_IN_BAG,
                       string_builder_dump(sb, NULL));
      string_builder_destroy(sb);
      return;
    }
  }

  // This is the total number of letters that are unseen from the nontarget's
  // perspective. We start with the total tiles in the bag and on both players'
  // racks, then subtract the letters known to be on the nontarget's rack.
  // After the loop below, this value equals: bag tiles + all tiles on the
  // target's rack + any unknown tiles on the nontarget's rack.
  int total_unseen_count = bag_get_letters(bag) +
                           rack_get_total_letters(player0_rack) +
                           rack_get_total_letters(player1_rack);
  for (int i = 0; i < ld_size; i++) {
    const int num_letter_i_on_nontarget_rack =
        rack_get_letter(args->nontarget_known_rack, i);
    bag_letter_counts[i] -= num_letter_i_on_nontarget_rack;
    if (bag_letter_counts[i] < 0) {
      StringBuilder *sb = string_builder_create();
      string_builder_add_string(sb, "noninferred player rack letters (");
      string_builder_add_rack(sb, args->nontarget_known_rack,
                              game_get_ld(game_dup), false);
      string_builder_add_string(sb, ") not available in the bag");
      error_stack_push(error_stack,
                       ERROR_STATUS_INFERENCE_NONTARGET_LETTERS_NOT_IN_BAG,
                       string_builder_dump(sb, NULL));
      string_builder_destroy(sb);
      return;
    }
    total_unseen_count -= num_letter_i_on_nontarget_rack;
  }

  const int num_played_letters =
      rack_get_total_letters(args->target_played_tiles);

  if (args->mode == INFERENCE_MODE_WINPCT && args->win_pcts == NULL) {
    error_stack_push(
        error_stack, ERROR_STATUS_INFERENCE_WINPCT_MISSING_WIN_PCTS,
        string_duplicate("win-probability inference requires a win-percentage "
                         "file (-wmp true)"));
    return;
  }

  if (args->mode == INFERENCE_MODE_WINPCT && args->use_game_history &&
      args->target_num_exch == 0 &&
      rack_is_empty(args->nontarget_known_rack)) {
    error_stack_push(
        error_stack, ERROR_STATUS_INFERENCE_WINPCT_MISSING_NONTARGET_RACK,
        string_duplicate(
            "win-probability tile-placement inference requires your rack to "
            "be known; use 'rack <tiles>' to set your rack before running "
            "'infer'"));
    return;
  }

  if (num_played_letters == 0 && args->target_num_exch == 0) {
    error_stack_push(
        error_stack, ERROR_STATUS_INFERENCE_NO_TILES_PLAYED,
        string_duplicate("cannot infer when no tiles are played or exchanged"));
    return;
  }

  if (num_played_letters != 0 && args->target_num_exch != 0) {
    error_stack_push(error_stack, ERROR_STATUS_INFERENCE_BOTH_PLAY_AND_EXCHANGE,
                     string_duplicate("cannot infer when both a tile placement "
                                      "and exchange move are specified"));
    return;
  }

  if (args->target_num_exch != 0 && total_unseen_count < (RACK_SIZE) * 2) {
    error_stack_push(error_stack, ERROR_STATUS_INFERENCE_EXCHANGE_NOT_ALLOWED,
                     get_formatted_string(
                         "cannot infer an exchange where there are fewer "
                         "than %d unseen tiles from the player's perspective",
                         (RACK_SIZE) * 2));
    return;
  }

  if (args->target_num_exch != 0 && args->target_score != 0) {
    error_stack_push(
        error_stack, ERROR_STATUS_INFERENCE_EXCHANGE_SCORE_NOT_ZERO,
        string_duplicate("cannot infer an exchange with a nonzero score"));
    return;
  }

  if (num_played_letters > (RACK_SIZE)) {
    error_stack_push(
        error_stack, ERROR_STATUS_INFERENCE_RACK_OVERFLOW,
        get_formatted_string("inferred player played more tiles (%d) "
                             "than can fit in a rack (%d)",
                             num_played_letters, RACK_SIZE));
    return;
  }
}

void populate_inference_args_with_game_history(InferenceArgs *args,
                                               Game *game_dup,
                                               ErrorStack *error_stack) {
  GameHistory *game_history = args->game_history;
  const int most_recent_move_event_index =
      game_history_get_most_recent_move_event_index(game_history);
  if (most_recent_move_event_index < 0) {
    error_stack_push(
        error_stack, ERROR_STATUS_INFERENCE_EMPTY_GAME_HISTORY,
        get_formatted_string(
            "cannot infer the previous play for an empty game history"));
    return;
  }
  const GameEvent *target_move_event =
      game_history_get_event(game_history, most_recent_move_event_index);
  const ValidatedMoves *last_move = game_event_get_vms(target_move_event);
  const Move *move = validated_moves_get_move(last_move, 0);
  const int move_tiles_length = move_get_tiles_length(move);
  rack_reset(args->target_played_tiles);
  for (int i = 0; i < move_tiles_length; i++) {
    if (move_get_tile(move, i) != PLAYED_THROUGH_MARKER) {
      if (get_is_blanked(move_get_tile(move, i))) {
        rack_add_letter(args->target_played_tiles, BLANK_MACHINE_LETTER);
      } else {
        rack_add_letter(args->target_played_tiles, move_get_tile(move, i));
      }
    }
  }
  args->target_index = game_event_get_player_index(target_move_event);
  args->target_score = game_event_get_move_score(target_move_event);
  args->target_num_exch = 0;
  if (move_get_type(move) == GAME_EVENT_EXCHANGE) {
    args->target_num_exch = move_get_tiles_played(move);
    rack_reset(args->target_played_tiles);
    args->target_move = NULL;
  } else {
    args->target_move = move;
  }
  // Only overwrite nontarget_known_rack from the replayed game state if the
  // caller has not already provided one. Callers such as analyze pre-set this
  // to the current player's actual GCG rack so that inference only considers
  // leaves whose tiles are genuinely available in the simulation bag. If we
  // always overwrote it here, we would copy the nontarget player's rack from
  // the replayed game_dup, which is empty (their tiles are in the bag at the
  // replayed state), allowing inference to enumerate racks that include tiles
  // already held by the current player — causing fatal draw failures when
  // simulation later tries to draw those tiles from the bag.
  if (rack_is_empty(args->nontarget_known_rack)) {
    rack_copy(
        args->nontarget_known_rack,
        player_get_rack(game_get_player(game_dup, 1 - args->target_index)));
  }
  if (rack_is_empty(args->target_known_rack)) {
    for (int i = most_recent_move_event_index - 1; i >= 0; i--) {
      GameEvent *event = game_history_get_event(game_history, i);
      if (game_event_get_player_index(event) == args->target_index) {
        rack_copy(args->target_known_rack,
                  game_event_get_after_event_player_off_turn_rack(event));
        break;
      }
    }
  }

  // This will play all of the events right up to but not including the target
  // move event
  game_play_n_events(game_history, game_dup, most_recent_move_event_index,
                     false, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
}

// ***************************
// Inference Context functions
// ***************************

InferenceCtx *inference_ctx_create(int num_threads) {
  InferenceCtx *ctx = calloc_or_die(1, sizeof(InferenceCtx));
  ctx->num_workers = num_threads;
  ctx->worker_inferences = calloc_or_die(ctx->num_workers, sizeof(Inference *));
  return ctx;
}

void inference_ctx_set_game(InferenceCtx *ctx, const Game *game) {
  if (ctx->game) {
    game_copy(ctx->game, game);
  } else {
    ctx->game = game_duplicate(game);
  }
}

// Assumes that ctx->game is set
void inference_ctx_set_inferences(InferenceCtx *ctx, const InferenceArgs *args,
                                  InferenceResults *results) {
  ctx->shared_rack_index = 0;
  inference_results_reset(results, args->leave_list_capacity,
                          ld_get_size(game_get_ld(args->game)));
  if (ctx->worker_inferences[0]) {
    for (int i = 0; i < ctx->num_workers; i++) {
      inference_reset(ctx->worker_inferences[i], ctx->game, args);
    }
  } else {
    cpthread_mutex_init(&ctx->shared_rack_index_lock);
    for (int i = 0; i < ctx->num_workers; i++) {
      ctx->worker_inferences[i] = inference_create(ctx->game, i, args, results);
      set_shared_variables_for_inference(ctx->worker_inferences[i],
                                         &ctx->shared_rack_index,
                                         &ctx->shared_rack_index_lock);
    }
    ctx->leave_stats = malloc_or_die((sizeof(Stat *)) * (ctx->num_workers));
    ctx->exchanged_stats = malloc_or_die((sizeof(Stat *)) * (ctx->num_workers));
    ctx->rack_stats = malloc_or_die((sizeof(Stat *)) * (ctx->num_workers));
  }
}

void inference_ctx_destroy(InferenceCtx *ctx) {
  if (!ctx) {
    return;
  }
  game_destroy(ctx->game);
  for (int i = 0; i < ctx->num_workers; i++) {
    inference_destroy(ctx->worker_inferences[i]);
  }
  free(ctx->worker_inferences);
  free(ctx->leave_stats);
  free(ctx->exchanged_stats);
  free(ctx->rack_stats);
  free(ctx);
}

void infer_with_initialized_ctx(InferenceArgs *args, InferenceCtx *ctx,
                                InferenceResults *results,
                                ErrorStack *error_stack) {
  inference_ctx_set_game(ctx, args->game);

  if (args->use_game_history) {
    populate_inference_args_with_game_history(args, ctx->game, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
  }

  verify_inference_args(args, ctx->game, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  inference_ctx_set_inferences(ctx, args, results);

  bool use_mc = false;
  if (args->mode == INFERENCE_MODE_WINPCT) {
    if (args->target_num_exch > 0) {
      // Exchange always uses MC (mirrors Macondo's isExchange short-circuit).
      use_mc = true;
    } else if (args->sample_mode == INFERENCE_SAMPLE_MODE_MC) {
      use_mc = true;
    } else if (args->sample_mode != INFERENCE_SAMPLE_MODE_ENUM) {
      // AUTO: enumerate when leaf space is small, MC otherwise.
      const Inference *w0 = ctx->worker_inferences[0];
      const int leave_size =
          (RACK_SIZE)-rack_get_total_letters(w0->current_target_rack);
      const int leaf_count =
          count_multisets(w0->bag_as_rack, leave_size, w0->ld_size);
      use_mc = (leaf_count > args->max_enum_hypotheses);
    }
  }

  if (use_mc) {
    infer_mc_manager(ctx, results);
  } else {
    infer_manager(ctx, results);
  }

  const Inference *wi = ctx->worker_inferences[0];
  inference_results_finalize(
      args->target_played_tiles, wi->current_target_leave, wi->bag_as_rack,
      results, wi->target_score, wi->target_number_of_tiles_exchanged,
      wi->equity_margin, args->mode,
      thread_control_get_status(args->thread_control) ==
          THREAD_CONTROL_STATUS_USER_INTERRUPT);
}

// Creates a new context if *ctx is NULL
// The caller is responsible for destroying the context
void infer(InferenceArgs *args, InferenceCtx **ctx, InferenceResults *results,
           ErrorStack *error_stack) {
  // The **ctx is a pointer to a pointer and must never be null;
  assert(ctx);
  if (*ctx == NULL) {
    *ctx = inference_ctx_create(args->num_threads);
  }
  infer_with_initialized_ctx(args, *ctx, results, error_stack);
}

void infer_without_ctx(InferenceArgs *args, InferenceResults *results,
                       ErrorStack *error_stack) {
  InferenceCtx *ctx = NULL;
  infer(args, &ctx, results, error_stack);
  inference_ctx_destroy(ctx);
}
