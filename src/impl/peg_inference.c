// Pre-endgame (PEG) inference: evaluates candidate opponent racks by running,
// for each candidate leave, one pre-endgame solve over the target's top moves
// to decide whether a PEG player with that rack would have made the observed
// play.
//
// This is the pre-endgame analogue of simmed_inference.c. The leave
// enumeration (exhaustive for small leaves, Monte Carlo for large), the static
// pre-filter, the logistic weight, and the result recording are all the same;
// only the inner "would they play this?" test differs: instead of an inner
// Monte Carlo sim it runs peg_solve over the candidate move set (only_moves)
// and compares the score+win utilities (sim_utility_blend of PEG win% and mean
// spread) of the best move and the observed move.

#include "peg_inference.h"

#include "../compat/ctime.h"
#include "../def/equity_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/move_defs.h"
#include "../def/rack_defs.h"
#include "../def/thread_control_defs.h"
#include "../ent/alias_method.h"
#include "../ent/bag.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/inference_args.h"
#include "../ent/inference_results.h"
#include "../ent/klv.h"
#include "../ent/leave_rack.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../ent/sim_args.h"
#include "../ent/thread_control.h"
#include "../ent/xoshiro.h"
#include "../impl/gameplay.h"
#include "../impl/inference.h"
#include "../impl/move_gen.h"
#include "../impl/peg.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SAMPLE_POOL 200
#define PEG_INFER_DEFAULT_CANDIDATE_PLAYS 7
// Utility-space margin. Pre-endgame utility gaps (win% blended with a sigmoid
// of spread, in [0, 1]) run well above the simmed inference's few-display-point
// margins, so the borderline point sits near a third of the scale. This is the
// primary weighting knob; tune it via the benchmark.
#define PEG_INFER_DEFAULT_UTILITY_MARGIN 0.3
#define PEG_INFER_DEFAULT_STATIC_PREFILTER 20.0

// ── Internal context ────────────────────────────────────────────────────────

typedef struct {
  // Base game: target has target_played+target_known tiles; nontarget has
  // nontarget_known tiles; bag = all_tiles - all known. game_copy'd into
  // inner_game for each candidate.
  Game *base_inner_game;
  Game *inner_game;

  Rack bag_as_rack;          // pool for candidate leave draws
  Rack current_leave;        // candidate leave being evaluated
  Rack target_known_combined; // target_played + target_known

  // Move / solver infrastructure reused across candidates.
  MoveList *inner_move_list;
  const Move **only_move_ptrs; // scratch: pointers into inner_move_list
  ThreadControl *inner_thread_control; // owned; for the inner peg_solve
  ErrorStack *inner_error_stack;       // owned

  // Observed move, extracted once.
  game_event_t observed_move_type;
  int observed_num_exch;
  Move observed_move_copy;

  int target_index;
  int ld_size;
  int leave_size;

  // Tuning.
  int num_candidate_plays;
  double util_w_winpct;
  double util_w_spread;
  double util_spread_scale;
  bool greedy_seed_only;
  int peg_max_stage;
  int peg_scenario_stride;
  PegOppModel peg_opp_model;
  double peg_time_budget_s;
  double time_budget_s;
  double peg_utility_margin;
  double static_prefilter_margin;

  // Not owned.
  ThreadControl *thread_control; // outer interrupt
  InferenceResults *results;
  int num_threads;

  XoshiroPRNG *prng;
  uint64_t eval_count;

  PegInferLeaveCallback leave_callback;
  void *leave_callback_data;
} PegInferCtx;

// ── Weight ──────────────────────────────────────────────────────────────────
//
// Smooth logistic on the utility gap, centred at the margin. gap <= 0 -> ~1.0,
// gap == margin -> 0.5, gap >> margin -> ~0.0. Same shape as simmed inference's
// compute_weight (gap and margin are both in [0, 1] utility units here).
static double compute_weight(double gap, double margin) {
  const double x = (gap - margin) / margin;
  return 1.0 / (1.0 + exp(3.0 * x));
}

// Score+win utility of one ranked candidate, on the simmer's [0, 1] scale.
static double peg_cand_utility(const PegInferCtx *ctx,
                               const PegRankedCand *cand) {
  return sim_utility_blend(cand->win_pct, double_to_equity(cand->mean_spread),
                           ctx->util_w_winpct, ctx->util_w_spread,
                           ctx->util_spread_scale);
}

// True when a ranked candidate is the observed move (exact tiles for a
// placement; matching exchange count for an exchange).
static bool cand_is_observed(const PegInferCtx *ctx, const Move *cand) {
  if (ctx->observed_move_type == GAME_EVENT_EXCHANGE) {
    return move_get_type(cand) == GAME_EVENT_EXCHANGE &&
           move_get_tiles_played(cand) == ctx->observed_num_exch;
  }
  return compare_moves_without_equity(cand, &ctx->observed_move_copy,
                                      /*allow_duplicates=*/true) == -1;
}

// ── Observed-move matching in the generated list ─────────────────────────────
static int find_observed_move_idx(const MoveList *ml, const PegInferCtx *ctx) {
  const int n = move_list_get_count(ml);
  if (ctx->observed_move_type == GAME_EVENT_EXCHANGE) {
    for (int i = 0; i < n; i++) {
      const Move *m = move_list_get_move(ml, i);
      if (move_get_type(m) == GAME_EVENT_EXCHANGE &&
          move_get_tiles_played(m) == ctx->observed_num_exch) {
        return i;
      }
    }
  } else {
    for (int i = 0; i < n; i++) {
      if (compare_moves_without_equity(move_list_get_move(ml, i),
                                       &ctx->observed_move_copy,
                                       /*allow_duplicates=*/true) == -1) {
        return i;
      }
    }
  }
  return -1;
}

// ── KLV-optimal exchange builder (verbatim from simmed inference) ────────────
static void make_best_exchange_move(const Rack *rack, int exch_count,
                                    const KLV *klv, int ld_size,
                                    Move *out_move) {
  MachineLetter flat[RACK_SIZE];
  int total = 0;
  for (int ml = 0; ml < ld_size && total < RACK_SIZE; ml++) {
    int cnt = (int)rack_get_letter(rack, ml);
    for (int k = 0; k < cnt && total < RACK_SIZE; k++) {
      flat[total++] = (MachineLetter)ml;
    }
  }

  move_set_type(out_move, GAME_EVENT_EXCHANGE);
  move_set_tiles_played(out_move, exch_count);
  move_set_tiles_length(out_move, exch_count);
  move_set_score(out_move, int_to_equity(0));

  const int keep_count = total - exch_count;
  if (keep_count <= 0) {
    for (int i = 0; i < total; i++) {
      move_set_tile(out_move, flat[i], i);
    }
    move_set_equity(out_move, int_to_equity(0));
    return;
  }

  int cur_idx[RACK_SIZE];
  int best_idx[RACK_SIZE];
  for (int i = 0; i < keep_count; i++) {
    cur_idx[i] = i;
    best_idx[i] = i;
  }
  Equity best_equity = 0;
  bool first = true;
  Rack leave_rack;
  rack_set_dist_size_and_reset(&leave_rack, ld_size);

  while (true) {
    rack_reset(&leave_rack);
    for (int i = 0; i < keep_count; i++) {
      rack_add_letter(&leave_rack, flat[cur_idx[i]]);
    }
    Equity eq = klv_get_leave_value(klv, &leave_rack);
    if (first || eq > best_equity) {
      best_equity = eq;
      for (int i = 0; i < keep_count; i++) {
        best_idx[i] = cur_idx[i];
      }
      first = false;
    }
    int pos = keep_count - 1;
    while (pos >= 0 && cur_idx[pos] == total - keep_count + pos) {
      pos--;
    }
    if (pos < 0) {
      break;
    }
    cur_idx[pos]++;
    for (int i = pos + 1; i < keep_count; i++) {
      cur_idx[i] = cur_idx[i - 1] + 1;
    }
  }

  bool kept[RACK_SIZE] = {false};
  for (int i = 0; i < keep_count; i++) {
    kept[best_idx[i]] = true;
  }
  int tile_idx = 0;
  for (int i = 0; i < total; i++) {
    if (!kept[i]) {
      move_set_tile(out_move, flat[i], tile_idx++);
    }
  }
  move_set_equity(out_move, best_equity);
}

// ── Core candidate evaluation ────────────────────────────────────────────────
//
// Rebuilds the target's pre-move rack (target_known_combined + current_leave),
// generates the top candidate moves, runs one peg_solve over them, and returns
// a weight in [0, 1] from the score+win utility gap.
static double peg_evaluate_candidate_leave(PegInferCtx *ctx) {
  // 1. Set up inner_game: copy base, add the candidate leave to the target's
  //    rack on top of the known tiles already drawn.
  game_copy(ctx->inner_game, ctx->base_inner_game);
  {
    Rack *target_rack =
        player_get_rack(game_get_player(ctx->inner_game, ctx->target_index));
    const int draw_idx =
        game_get_player_draw_index(ctx->inner_game, ctx->target_index);
    Bag *bag = game_get_bag(ctx->inner_game);
    for (int i = 0; i < ctx->ld_size; i++) {
      const uint16_t n = rack_get_letter(&ctx->current_leave, i);
      for (uint16_t j = 0; j < n; j++) {
        if (!bag_draw_letter(bag, i, draw_idx)) {
          return 0.0;
        }
        rack_add_letter(target_rack, i);
      }
    }
  }

  // 2. Generate top num_candidate_plays moves by static equity.
  move_list_reset(ctx->inner_move_list);
  const MoveGenArgs gen_args = {
      .game = ctx->inner_game,
      .move_list = ctx->inner_move_list,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      .tiles_played_bv = NULL,
  };
  generate_moves(&gen_args);

  move_list_sort_moves(ctx->inner_move_list);
  if (move_list_get_count(ctx->inner_move_list) > ctx->num_candidate_plays) {
    ctx->inner_move_list->count = ctx->num_candidate_plays;
  }
  const int num_static = move_list_get_count(ctx->inner_move_list);
  if (num_static == 0) {
    return 0.0;
  }

  // 3. Find (or build) the observed move's slot in the candidate list.
  int obs_idx = -1;
  if (ctx->observed_move_type == GAME_EVENT_EXCHANGE) {
    const Rack *target_rack =
        player_get_rack(game_get_player(ctx->inner_game, ctx->target_index));
    const int rack_total = rack_get_total_letters(target_rack);
    if (rack_total < ctx->observed_num_exch) {
      return 0.0;
    }
    const KLV *klv =
        player_get_klv(game_get_player(ctx->inner_game, ctx->target_index));
    // Exchanging is legal only when the bag holds at least a full rack
    // (RACK_SIZE) -- you must be able to draw a replacement for every tile you
    // could return. In the PEG bag range [PEG_MIN_BAG, PEG_MAX_BAG] this never
    // holds, so no exchange arms are built; the branch is kept correct for a
    // boundary reconstruction whose bag is >= RACK_SIZE. An exchange observed
    // in a sub-full-rack position is impossible, so obs_idx stays -1 and the
    // leave scores 0.
    int n_arms = 0;
    if (bag_get_letters(game_get_bag(ctx->inner_game)) >= RACK_SIZE) {
      for (int k = 1; k <= rack_total; k++) {
        Move *slot = ctx->inner_move_list->moves[num_static + n_arms];
        make_best_exchange_move(target_rack, k, klv, ctx->ld_size, slot);
        if (k == ctx->observed_num_exch) {
          obs_idx = num_static + n_arms;
        }
        n_arms++;
      }
    }
    ctx->inner_move_list->count = num_static + n_arms;
  } else {
    obs_idx = find_observed_move_idx(ctx->inner_move_list, ctx);
    if (obs_idx < 0) {
      Move *extra = ctx->inner_move_list->moves[num_static];
      move_copy(extra, &ctx->observed_move_copy);
      ctx->inner_move_list->count = num_static + 1;
      obs_idx = num_static;
    }
  }
  if (obs_idx < 0) {
    return 0.0;
  }
  if (ctx->inner_move_list->count == 1) {
    return 1.0; // only one legal move; trivially consistent
  }

  const int inner_arms = ctx->inner_move_list->count;
  double gap = 0.0;
  double result = 0.0;

  // 4. Static pre-filter: skip the (expensive) PEG solve when the observed move
  //    already trails the static best by a wide margin. Kept generous, since a
  //    low-static setup can still be the pre-endgame win% best.
  if (ctx->leave_size > 1) {
    const double best_static = equity_to_double(
        move_get_equity(move_list_get_move(ctx->inner_move_list, 0)));
    const double obs_static = equity_to_double(
        move_get_equity(move_list_get_move(ctx->inner_move_list, obs_idx)));
    if (best_static - obs_static > ctx->static_prefilter_margin * 4.0) {
      result = 0.0;
      goto invoke_callback;
    }
  }

  // 5. Run one PEG solve over the candidate set and read each move's utility.
  for (int i = 0; i < inner_arms; i++) {
    ctx->only_move_ptrs[i] = move_list_get_move(ctx->inner_move_list, i);
  }
  const Move *protect_arr[1] = {
      move_list_get_move(ctx->inner_move_list, obs_idx)};

  ctx->eval_count++;
  thread_control_set_status(ctx->inner_thread_control,
                            THREAD_CONTROL_STATUS_STARTED);
  PegArgs peg_args = {0};
  peg_args.game = ctx->inner_game;
  peg_args.thread_control = ctx->inner_thread_control;
  peg_args.num_threads = ctx->num_threads;
  peg_args.time_budget_seconds = ctx->peg_time_budget_s;
  peg_args.greedy_seed_only = ctx->greedy_seed_only;
  peg_args.max_stage = ctx->peg_max_stage;
  peg_args.scenario_stride = ctx->peg_scenario_stride;
  peg_args.opp_model = ctx->peg_opp_model;
  peg_args.only_moves = ctx->only_move_ptrs;
  peg_args.n_only_moves = inner_arms;
  peg_args.protect_moves = protect_arr;
  peg_args.n_protect_moves = 1;

  PegResult peg_result = {0};
  error_stack_reset(ctx->inner_error_stack);
  peg_solve(&peg_args, &peg_result, ctx->inner_error_stack);

  if (error_stack_is_empty(ctx->inner_error_stack) &&
      peg_result.n_top_cands > 0 && peg_result.best_win >= 0.0) {
    double best_util = -1.0;
    double obs_util = -1.0;
    for (int i = 0; i < peg_result.n_top_cands; i++) {
      const double u = peg_cand_utility(ctx, &peg_result.top_cands[i]);
      if (u > best_util) {
        best_util = u;
      }
      if (obs_util < 0.0 && cand_is_observed(ctx, &peg_result.top_cands[i].move)) {
        obs_util = u;
      }
    }
    if (obs_util >= 0.0) {
      gap = best_util - obs_util;
      result = compute_weight(gap, ctx->peg_utility_margin);
    } else {
      // Observed move not in the ranked set (should not happen with
      // protect_moves): no usable signal, skip this leave.
      result = 0.0;
    }
  } else {
    // Solve produced no usable ranking (e.g. cut off before any candidate
    // finished): skip this leave rather than guess.
    result = 0.0;
  }
  peg_result_destroy(&peg_result);

invoke_callback:
  if (ctx->leave_callback) {
    ctx->leave_callback(&ctx->current_leave, ctx->inner_move_list, obs_idx, gap,
                        result, ctx->inner_game, ctx->leave_callback_data);
  }
  return result;
}

// ── Recording (verbatim from simmed inference) ───────────────────────────────
static void record_candidate_leave(PegInferCtx *ctx, double weight) {
  if (weight <= 0.0) {
    return;
  }
  uint64_t raw_draws =
      get_number_of_draws_for_rack(&ctx->bag_as_rack, &ctx->current_leave);
  if (raw_draws == 0) {
    return;
  }
  uint64_t weighted = (uint64_t)(raw_draws * weight + 0.5);
  if (weighted == 0) {
    return;
  }
  const KLV *klv =
      player_get_klv(game_get_player(ctx->base_inner_game, ctx->target_index));
  Equity leave_equity = klv_get_leave_value(klv, &ctx->current_leave);

  record_valid_leave(&ctx->current_leave, ctx->results, INFERENCE_TYPE_LEAVE,
                     equity_to_double(leave_equity), weighted);
  alias_method_add_rack(inference_results_get_alias_method(ctx->results),
                        &ctx->current_leave, (int)weighted);
  leave_rack_list_insert_rack(
      &ctx->current_leave, /*exchanged=*/NULL, (int)weighted, leave_equity,
      inference_results_get_leave_rack_list(ctx->results));
}

static void evaluate_and_record(PegInferCtx *ctx) {
  if (thread_control_get_status(ctx->thread_control) ==
      THREAD_CONTROL_STATUS_USER_INTERRUPT) {
    return;
  }
  const double weight = peg_evaluate_candidate_leave(ctx);
  record_candidate_leave(ctx, weight);
}

// ── Exhaustive enumeration (verbatim from simmed inference) ──────────────────
static void exhaust_leaves(PegInferCtx *ctx, int tiles_to_place,
                           int start_letter) {
  if (thread_control_get_status(ctx->thread_control) ==
      THREAD_CONTROL_STATUS_USER_INTERRUPT) {
    return;
  }
  if (tiles_to_place == 0) {
    evaluate_and_record(ctx);
    return;
  }
  for (int letter = start_letter; letter < ctx->ld_size; letter++) {
    if (rack_get_letter(&ctx->bag_as_rack, letter) > 0) {
      rack_take_letter(&ctx->bag_as_rack, letter);
      rack_add_letter(&ctx->current_leave, letter);
      exhaust_leaves(ctx, tiles_to_place - 1, letter);
      rack_take_letter(&ctx->current_leave, letter);
      rack_add_letter(&ctx->bag_as_rack, letter);
    }
  }
}

// ── Monte Carlo leave sampling (verbatim from simmed inference) ──────────────
static void sample_leave_mc(PegInferCtx *ctx) {
  MachineLetter pool[MAX_SAMPLE_POOL];
  int total = 0;
  for (int i = 0; i < ctx->ld_size && total < MAX_SAMPLE_POOL; i++) {
    int cnt = rack_get_letter(&ctx->bag_as_rack, i);
    for (int j = 0; j < cnt && total < MAX_SAMPLE_POOL; j++) {
      pool[total++] = (MachineLetter)i;
    }
  }
  rack_reset(&ctx->current_leave);
  const int n = ctx->leave_size < total ? ctx->leave_size : total;
  for (int i = 0; i < n; i++) {
    uint64_t j =
        (uint64_t)i + prng_get_random_number(ctx->prng, (uint64_t)(total - i));
    MachineLetter tmp = pool[i];
    pool[i] = pool[j];
    pool[j] = tmp;
    rack_add_letter(&ctx->current_leave, pool[i]);
  }
}

// ── Base game setup (verbatim from simmed inference) ─────────────────────────
static bool setup_base_inner_game(PegInferCtx *ctx,
                                  const PegInferenceArgs *args,
                                  ErrorStack *error_stack) {
  const InferenceArgs *base = args->base;
  ctx->base_inner_game = game_duplicate(base->game);
  return_rack_to_bag(ctx->base_inner_game, 0);
  return_rack_to_bag(ctx->base_inner_game, 1);

  rack_set_dist_size_and_reset(&ctx->target_known_combined, ctx->ld_size);
  rack_union(&ctx->target_known_combined, base->target_played_tiles);
  rack_union(&ctx->target_known_combined, base->target_known_rack);

  if (!draw_rack_from_bag(ctx->base_inner_game, ctx->target_index,
                          &ctx->target_known_combined)) {
    error_stack_push(
        error_stack, ERROR_STATUS_INFERENCE_TARGET_LETTERS_NOT_IN_BAG,
        string_duplicate(
            "peg inference: failed to draw combined target tiles from bag"));
    return false;
  }
  if (!draw_rack_from_bag(ctx->base_inner_game, 1 - ctx->target_index,
                          base->nontarget_known_rack)) {
    log_fatal("peg inference: failed to draw nontarget known rack from bag");
  }

  const Bag *bag = game_get_bag(ctx->base_inner_game);
  int bag_counts[MAX_ALPHABET_SIZE];
  memset(bag_counts, 0, sizeof(bag_counts));
  bag_increment_unseen_count(bag, bag_counts);
  rack_set_dist_size_and_reset(&ctx->bag_as_rack, ctx->ld_size);
  for (int i = 0; i < MAX_ALPHABET_SIZE; i++) {
    rack_add_letters(&ctx->bag_as_rack, i, bag_counts[i]);
  }
  return true;
}

// ── Entry point ──────────────────────────────────────────────────────────────
void peg_infer(const PegInferenceArgs *args, InferenceResults *results,
               ErrorStack *error_stack) {
  const InferenceArgs *base = args->base;

  if (!args->observed_move) {
    error_stack_push(error_stack, ERROR_STATUS_INFERENCE_NO_TILES_PLAYED,
                     string_duplicate("peg_infer: no observed move provided"));
    return;
  }

  const int known_target_tiles =
      rack_get_total_letters(base->target_played_tiles) +
      rack_get_total_letters(base->target_known_rack);
  const int leave_size = RACK_SIZE - known_target_tiles;
  if (leave_size < 0) {
    error_stack_push(
        error_stack, ERROR_STATUS_INFERENCE_RACK_OVERFLOW,
        string_duplicate("peg_infer: more known target tiles than rack size"));
    return;
  }

  PegInferCtx ctx;
  memset(&ctx, 0, sizeof(ctx));

  ctx.target_index = base->target_index;
  ctx.ld_size = ld_get_size(game_get_ld(base->game));
  ctx.leave_size = leave_size;
  ctx.num_candidate_plays = args->num_candidate_plays > 0
                                ? args->num_candidate_plays
                                : PEG_INFER_DEFAULT_CANDIDATE_PLAYS;
  ctx.util_w_winpct =
      args->utility_w_winpct > 0.0 ? args->utility_w_winpct : 1.0;
  ctx.util_w_spread = args->utility_w_spread; // 0.0 default = pure win%
  ctx.util_spread_scale =
      args->utility_spread_scale > 0.0 ? args->utility_spread_scale : 100.0;
  ctx.greedy_seed_only = args->greedy_seed_only;
  ctx.peg_max_stage = args->peg_max_stage;
  ctx.peg_scenario_stride = args->peg_scenario_stride;
  ctx.peg_opp_model = args->peg_opp_model;
  ctx.peg_time_budget_s = args->peg_time_budget_s;
  ctx.time_budget_s = args->time_budget_s;
  ctx.peg_utility_margin = args->peg_utility_margin > 0.0
                               ? args->peg_utility_margin
                               : PEG_INFER_DEFAULT_UTILITY_MARGIN;
  ctx.static_prefilter_margin = args->static_prefilter_margin > 0.0
                                    ? args->static_prefilter_margin
                                    : PEG_INFER_DEFAULT_STATIC_PREFILTER;
  ctx.thread_control = base->thread_control;
  ctx.results = results;
  ctx.num_threads = base->num_threads;
  ctx.eval_count = 0;

  ctx.observed_move_type = move_get_type(args->observed_move);
  ctx.observed_num_exch = base->target_num_exch;
  move_copy(&ctx.observed_move_copy, args->observed_move);
  ctx.leave_callback = args->leave_callback;
  ctx.leave_callback_data = args->leave_callback_data;

  rack_set_dist_size_and_reset(&ctx.bag_as_rack, ctx.ld_size);
  rack_set_dist_size_and_reset(&ctx.current_leave, ctx.ld_size);
  rack_set_dist_size_and_reset(&ctx.target_known_combined, ctx.ld_size);

  if (!setup_base_inner_game(&ctx, args, error_stack)) {
    return;
  }

  inference_results_reset(results, base->leave_list_capacity, ctx.ld_size);

  const int move_cap = ctx.num_candidate_plays + RACK_SIZE;
  ctx.inner_move_list = move_list_create(move_cap);
  ctx.only_move_ptrs = malloc_or_die((size_t)move_cap * sizeof(const Move *));
  ctx.inner_thread_control = thread_control_create();
  ctx.inner_error_stack = error_stack_create();
  ctx.inner_game = game_duplicate(ctx.base_inner_game);
  ctx.prng = prng_create(42);

  const int exhaustive_max = args->exhaustive_max_leave > 0
                                 ? args->exhaustive_max_leave
                                 : PEG_INFER_EXHAUSTIVE_MAX_DEFAULT;

  if (leave_size == 0) {
    rack_reset(&ctx.current_leave);
    evaluate_and_record(&ctx);
  } else if (leave_size <= exhaustive_max) {
    exhaust_leaves(&ctx, leave_size, BLANK_MACHINE_LETTER);
  } else {
    Timer mc_timer;
    ctimer_reset(&mc_timer);
    ctimer_start(&mc_timer);
    while (thread_control_get_status(ctx.thread_control) !=
               THREAD_CONTROL_STATUS_USER_INTERRUPT &&
           ctimer_elapsed_seconds(&mc_timer) < ctx.time_budget_s) {
      sample_leave_mc(&ctx);
      evaluate_and_record(&ctx);
    }
  }

  inference_results_finalize(base->target_played_tiles, base->target_known_rack,
                             &ctx.bag_as_rack, results, base->target_score,
                             base->target_num_exch, base->equity_margin,
                             thread_control_get_status(ctx.thread_control) ==
                                 THREAD_CONTROL_STATUS_USER_INTERRUPT);

  prng_destroy(ctx.prng);
  error_stack_destroy(ctx.inner_error_stack);
  thread_control_destroy(ctx.inner_thread_control);
  free(ctx.only_move_ptrs);
  move_list_destroy(ctx.inner_move_list);
  game_destroy(ctx.inner_game);
  game_destroy(ctx.base_inner_game);
}
