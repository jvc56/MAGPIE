// Simulation-based inference: evaluates candidate opponent racks by running
// short inner sims to determine whether a sim player with that rack would
// have made the observed play.
//
// Compared to static inference (which uses KLV leave values), simmed inference
// correctly handles setup plays whose sim equity exceeds their static equity.
//
// Architecture:
//   - Outer loop: exhaustive enumeration (leave_size <= SIMMED_INFER_EXHAUSTIVE_MAX)
//     or Monte Carlo sampling (larger leaves).
//   - Inner evaluation: two-stage probe + full sim with early stopping.
//   - Weight: smooth logistic on (best_sim_equity - obs_sim_equity); hedges
//     uncertain cases with a midlevel weight rather than binary accept/reject.
//   - Threading: all num_threads are devoted to each inner sim; the outer loop
//     is single-threaded.

#include "simmed_inference.h"

#include "../compat/ctime.h"
#include "../def/equity_defs.h"
#include "../def/game_history_defs.h"
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
#include "../ent/sim_results.h"
#include "../ent/stats.h"
#include "../ent/thread_control.h"
#include "../ent/win_pct.h"
#include "../ent/xoshiro.h"
#include "../impl/gameplay.h"
#include "../impl/inference.h"
#include "../impl/move_gen.h"
#include "../impl/simmer.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Conservative upper bound on tiles that could be in the leave pool.
// Standard Scrabble has 100 tiles; even with small racks and bags this fits.
#define MAX_SAMPLE_POOL 200

// ── Internal context ──────────────────────────────────────────────────────────

typedef struct {
  // Base game: target has target_played+target_known tiles; nontarget has
  // nontarget_known tiles; bag = all_tiles - target_played - target_known
  // - nontarget_known. game_copy'd into inner_game for each candidate.
  Game *base_inner_game;

  // Working copy reset for each candidate evaluation.
  Game *inner_game;

  // Pool of tiles available for leave draws
  // (= all tiles minus target's known tiles and nontarget's known tiles).
  Rack bag_as_rack;

  // The candidate leave being evaluated (mutated during recursion / MC).
  Rack current_leave;

  // Combined known target tiles = target_played + target_known.
  Rack target_known_combined;

  // Sim infrastructure (reused across candidates).
  MoveList *inner_move_list;
  SimResults *inner_sim_results;
  SimCtx *inner_sim_ctx;
  ErrorStack *inner_error_stack;

  // Observed move fields, extracted once at setup.
  game_event_t observed_move_type;
  int observed_num_exch;
  Move observed_move_copy; // for exact tile-placement matching

  int target_index;
  int ld_size;
  int leave_size;

  // Tuning parameters.
  int num_candidate_plays;
  int num_inner_sim_plies;
  int probe_iterations;
  int full_iterations;
  double time_budget_s;
  double sim_equity_margin;

  // Not owned.
  ThreadControl *thread_control;
  WinPct *win_pcts;
  InferenceResults *results;
  int num_threads;

  // PRNG for Monte Carlo leave sampling.
  XoshiroPRNG *prng;

  // Monotonically increasing counter used as inner-sim seed.
  uint64_t eval_count;

  // Optional callback invoked after each candidate leave evaluation.
  SimmedInferLeaveCallback leave_callback;
  void *leave_callback_data;
} SimmedInferCtx;

// ── Bogopoints ────────────────────────────────────────────────────────────────
//
// Converts a win-percentage gap to "bogopoints": the integer number of display
// points X such that win_pct_get(wp, X, avg_unseen) − win_pct_get(wp, 0,
// avg_unseen) ≈ wp_gap.  Uses equity_gap_pts as a fallback when both plays are
// pinned at the same win% extreme (100%/0%) and the gap cannot be expressed in
// the win_pct table.
static double win_pct_gap_to_bogopoints(double wp_gap, int avg_unseen,
                                        const WinPct *wp,
                                        double equity_gap_pts) {
  if (wp_gap <= 0.0) {
    return 0.0;
  }
  // Clamp avg_unseen to the table's range.
  const int max_unseen = win_pct_get_max_tiles_unseen(wp);
  if (avg_unseen < 1) {
    avg_unseen = 1;
  }
  if (avg_unseen > max_unseen) {
    avg_unseen = max_unseen;
  }

  const double base_wp = (double)win_pct_get(wp, 0, (unsigned int)avg_unseen);
  // win_pct_get clamps to max_spread, so 9999 gives the maximum win%.
  const double max_wp = (double)win_pct_get(wp, 9999, (unsigned int)avg_unseen);
  const double target_wp = base_wp + wp_gap;

  // If target_wp is at or above the achievable maximum, fall back to equity.
  if (target_wp >= max_wp) {
    return equity_gap_pts;
  }

  // Binary search on integer display points.
  int lo = 0, hi = 9999;
  while (lo < hi) {
    const int mid = lo + (hi - lo) / 2;
    if ((double)win_pct_get(wp, mid, (unsigned int)avg_unseen) < target_wp) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return (double)lo;
}

// ── Weight function ───────────────────────────────────────────────────────────
//
// gap = bogopoints (display points derived from win% gap, may be negative)
//
// Returns weight in [0, 1]:
//   gap <= 0              → ~1.0  (observed is the sim best)
//   gap = sim_margin      →  0.5  (at the margin)
//   gap = 2 * sim_margin  → ~0.05 (nearly rejected)
//   gap >> sim_margin     → ~0.0
//
// Logistic centred at sim_margin with scale = sim_margin.
static double compute_weight(double gap, double sim_margin) {
  double x = (gap - sim_margin) / sim_margin;
  return 1.0 / (1.0 + exp(3.0 * x));
}

// ── Observed-move matching ────────────────────────────────────────────────────
//
// For tile placements: finds the index of the exact position+tile match.
// For exchanges:       finds the first (highest-equity) exchange of the
//                      observed count.
// Returns -1 if no match exists (play impossible with this rack).
static int find_observed_move_idx(const MoveList *ml,
                                  const SimmedInferCtx *ctx) {
  const int n = move_list_get_count(ml);
  if (ctx->observed_move_type == GAME_EVENT_EXCHANGE) {
    for (int i = 0; i < n; i++) {
      const Move *m = move_list_get_move(ml, i);
      if (move_get_type(m) == GAME_EVENT_EXCHANGE &&
          move_get_tiles_played(m) == ctx->observed_num_exch) {
        return i; // list is sorted descending; first match is best exchange
      }
    }
  } else {
    for (int i = 0; i < n; i++) {
      // compare_moves_without_equity returns -1 when moves are identical
      if (compare_moves_without_equity(move_list_get_move(ml, i),
                                       &ctx->observed_move_copy,
                                       /*allow_duplicates=*/true) == -1) {
        return i;
      }
    }
  }
  return -1;
}

// ── KLV-optimal exchange builder ──────────────────────────────────────────────
//
// Enumerates all C(total, exch_count) ways to split the rack and keeps the
// split that maximises the KLV value of the leave (kept tiles).  The exchanged
// tiles — the complement — are stored in out_move.
//
// For 7 tiles this is at most C(7,3)=C(7,4)=35 iterations; each iteration
// costs one KLV lookup (fast hash).  Total cost is negligible.
static void make_best_exchange_move(const Rack *rack, int exch_count,
                                    const KLV *klv, int ld_size,
                                    Move *out_move) {
  // Flatten rack to an array of machine letters.
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
    // Exchange everything — no leave to optimise.
    for (int i = 0; i < total; i++) {
      move_set_tile(out_move, flat[i], i);
    }
    move_set_equity(out_move, int_to_equity(0));
    return;
  }

  // Enumerate all C(total, keep_count) index combinations.
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
    // Advance to the next combination (standard algorithm).
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

  // Exchange tiles = everything in flat[] NOT at a best_idx position.
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

// ── Core candidate evaluation ─────────────────────────────────────────────────
//
// Adds current_leave to the target's rack in inner_game (on top of the
// target_known_combined tiles already drawn), generates moves, and runs a
// two-stage probe/full sim. Returns a weight in [0, 1] representing how
// consistent this candidate rack is with the observed play.
static double sim_evaluate_candidate_leave(SimmedInferCtx *ctx) {
  // 1. Set up inner_game: copy base, then add the candidate leave to the
  //    target's rack. base_inner_game already has target_known_combined
  //    (played + known tiles) drawn for the target; we ADD current_leave on
  //    top so the target has their full rack (target_known_combined +
  //    current_leave) and the observed move's tiles are always present.
  //    draw_rack_from_bag would wrongly REPLACE the rack with only the leave,
  //    causing rack_take_letter underflows when the observed move is played.
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
          return 0.0; // shouldn't happen if current_leave sampled from bag_as_rack
        }
        rack_add_letter(target_rack, i);
      }
    }
  }

  // 2. Generate top num_candidate_plays moves by static equity.
  //    The inner_move_list has capacity num_candidate_plays+RACK_SIZE so there
  //    is always room to add the observed move and exchange arms.
  move_list_reset(ctx->inner_move_list);
  const MoveGenArgs gen_args = {
      .game = ctx->inner_game,
      .move_list = ctx->inner_move_list,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .thread_index = 0,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      .tiles_played_bv = NULL,
  };
  generate_moves(&gen_args);

  // Sort first (heap → descending equity), then trim to top num_candidate_plays.
  // Sorting before trimming is critical: the heap does not store elements in
  // equity order, so trimming first would keep an arbitrary 5 rather than the
  // actual top 5.
  move_list_sort_moves(ctx->inner_move_list);
  if (move_list_get_count(ctx->inner_move_list) > ctx->num_candidate_plays) {
    ctx->inner_move_list->count = ctx->num_candidate_plays;
  }
  const int num_static = move_list_get_count(ctx->inner_move_list);
  if (num_static == 0) {
    return 0.0;
  }

  // 3. Find (or build) the observed move's slot in the candidate list.
  //
  // For exchanges: always append one KLV-optimal exchange arm for every
  // possible leave size (exchange counts 1..rack_total), so the inner sim
  // compares the observed N-tile exchange against all other exchanges AND
  // all scoring plays.  obs_idx is set to the arm whose count matches.
  //
  // For tile placements: try to find the exact observed move in the top-N;
  // if absent (e.g. a setup play with low static equity), force-insert it
  // into the reserved extra slot.
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
    for (int k = 1; k <= rack_total; k++) {
      Move *slot = ctx->inner_move_list->moves[num_static + k - 1];
      make_best_exchange_move(target_rack, k, klv, ctx->ld_size, slot);
      if (k == ctx->observed_num_exch) {
        obs_idx = num_static + k - 1;
      }
    }
    ctx->inner_move_list->count = num_static + rack_total;
  } else {
    // For tile placements, the observed move IS playable (target tiles include
    // all played tiles), so the only thing missing is its ranking.
    obs_idx = find_observed_move_idx(ctx->inner_move_list, ctx);
    if (obs_idx < 0) {
      Move *extra = ctx->inner_move_list->moves[num_static];
      move_copy(extra, &ctx->observed_move_copy);
      ctx->inner_move_list->count = num_static + 1;
      obs_idx = num_static;
    }
  }
  if (obs_idx < 0) {
    return 0.0; // observed move not playable with this rack
  }
  if (ctx->inner_move_list->count == 1) {
    return 1.0; // only one legal move; trivially consistent
  }

  // Declare here so all paths below see them (required before any goto).
  const int inner_arms = ctx->inner_move_list->count;
  double gap = 0.0;
  double result = 0.0;

  // 4. Static pre-filter: for leaves larger than 1 tile, skip the inner sim
  //    when the observed move is already well behind the static best. 1-tile
  //    leaves have at most ~33 candidates so they always sim. Larger leaves
  //    can have thousands of candidates and this avoids most probe sims.
  if (ctx->leave_size > 1) {
    const double best_static = equity_to_double(
        move_get_equity(move_list_get_move(ctx->inner_move_list, 0)));
    const double obs_static = equity_to_double(
        move_get_equity(move_list_get_move(ctx->inner_move_list, obs_idx)));
    gap = best_static - obs_static;
    if (gap > ctx->sim_equity_margin * 4.0) {
      result = 0.0;
      // Reset sim results so the callback sees iters=0 (no sim ran).
      // Without this, inner_sim_results may hold stale data from a previous
      // candidate with a different move-list size, causing out-of-bounds access.
      sim_results_reset(ctx->inner_move_list, ctx->inner_sim_results,
                        ctx->num_inner_sim_plies, ctx->eval_count,
                        /*use_heat_map=*/false);
      goto invoke_callback;
    }
  }

  // 5. Run inner sim in two stages. Uses a local macro to avoid repeating the
  //    sim setup. The inner sim always uses known_opp_rack=NULL so the target's
  //    simulated opponent is sampled uniformly from available tiles.
#define RUN_INNER_SIM(max_iters, gap_out)                                      \
  do {                                                                         \
    ctx->eval_count++;                                                         \
    thread_control_set_status(ctx->thread_control,                             \
                              THREAD_CONTROL_STATUS_STARTED);                  \
    SimArgs _sa;                                                               \
    sim_args_fill(ctx->num_inner_sim_plies, ctx->inner_move_list,              \
                  /*known_opp_rack=*/NULL, ctx->win_pcts,                      \
                  /*inference_results=*/NULL, ctx->thread_control,             \
                  ctx->inner_game,                                             \
                  /*use_inference=*/false,                                     \
                  /*use_heat_map=*/false, ctx->num_threads,                    \
                  /*print_interval=*/0,                                        \
                  /*max_display_plays=*/inner_arms,                            \
                  /*max_display_plies=*/ctx->num_inner_sim_plies,              \
                  /*seed=*/ctx->eval_count,                                    \
                  /*max_iterations=*/(uint64_t)(max_iters),                   \
                  /*min_play_iterations=*/1,                                   \
                  /*scond=*/101.0, BAI_THRESHOLD_NONE,                        \
                  /*time_limit_seconds=*/9999,                                 \
                  BAI_SAMPLING_RULE_ROUND_ROBIN,                               \
                  /*cutoff=*/0.0, /*inference_args=*/NULL, &_sa);              \
    error_stack_reset(ctx->inner_error_stack);                                 \
    simulate(&_sa, &ctx->inner_sim_ctx, ctx->inner_sim_results,               \
             ctx->inner_error_stack);                                          \
    /* Find best arm by win% (not equity) */                                   \
    int _best_arm = 0;                                                         \
    double _best_wp_val = -1.0;                                                \
    for (int _i = 0; _i < inner_arms; _i++) {                                 \
      const double _wp = stat_get_mean(simmed_play_get_win_pct_stat(           \
          sim_results_get_simmed_play(ctx->inner_sim_results, _i)));           \
      if (_wp > _best_wp_val) {                                                \
        _best_wp_val = _wp;                                                    \
        _best_arm = _i;                                                        \
      }                                                                        \
    }                                                                          \
    const double _obs_wp = stat_get_mean(simmed_play_get_win_pct_stat(         \
        sim_results_get_simmed_play(ctx->inner_sim_results, obs_idx)));        \
    const double _wp_gap = _best_wp_val - _obs_wp;                             \
    /* Equity gap used as fallback for bogopoints when both plays are pinned */\
    const double _best_eq = stat_get_mean(simmed_play_get_equity_stat(         \
        sim_results_get_simmed_play(ctx->inner_sim_results, _best_arm)));      \
    const double _obs_eq = stat_get_mean(simmed_play_get_equity_stat(          \
        sim_results_get_simmed_play(ctx->inner_sim_results, obs_idx)));        \
    /* Avg unseen tiles: bag + opponent rack before any move is executed */    \
    const int _bag_sz = bag_get_letters(game_get_bag(ctx->inner_game));        \
    const int _opp_sz = rack_get_total_letters(                                \
        player_get_rack(game_get_player(ctx->inner_game,                       \
                                       1 - ctx->target_index)));               \
    int _avg_unseen = _bag_sz + _opp_sz;                                       \
    if (_avg_unseen < 1) _avg_unseen = 1;                                      \
    (gap_out) = win_pct_gap_to_bogopoints(_wp_gap, _avg_unseen,                \
                                          ctx->win_pcts, _best_eq - _obs_eq);  \
  } while (0)

  RUN_INNER_SIM(ctx->probe_iterations, gap);

  // Respect outer interrupt (e.g. total inference time budget exhausted).
  if (thread_control_get_status(ctx->thread_control) ==
      THREAD_CONTROL_STATUS_USER_INTERRUPT) {
    result = compute_weight(gap, ctx->sim_equity_margin);
    goto invoke_callback;
  }

  // Early reject: gap far above margin even after a rough probe.
  if (gap > ctx->sim_equity_margin * 4.0) {
    result = 0.0;
    goto invoke_callback;
  }

  // Early accept: observed play is the sim best (or better).
  if (gap <= 0.0) {
    result = 1.0;
    goto invoke_callback;
  }

  // Uncertain: run the full evaluation for a more reliable estimate.
  RUN_INNER_SIM(ctx->full_iterations, gap);

#undef RUN_INNER_SIM

  result = compute_weight(gap, ctx->sim_equity_margin);
  if (thread_control_get_status(ctx->thread_control) ==
      THREAD_CONTROL_STATUS_USER_INTERRUPT) {
    goto invoke_callback; // result already set above
  }

invoke_callback:
  if (ctx->leave_callback) {
    ctx->leave_callback(&ctx->current_leave, ctx->inner_move_list,
                        ctx->inner_sim_results, obs_idx, gap, result,
                        ctx->inner_game, ctx->leave_callback_data);
  }
  return result;
}

// ── Recording a candidate leave ───────────────────────────────────────────────
//
// Weights the leave's combinatorial draw count by the sim acceptance weight
// and records it into InferenceResults.
static void record_candidate_leave(SimmedInferCtx *ctx, double weight) {
  if (weight <= 0.0) {
    return;
  }

  uint64_t raw_draws =
      get_number_of_draws_for_rack(&ctx->bag_as_rack, &ctx->current_leave);
  if (raw_draws == 0) {
    return;
  }

  // Scale draw count by sim acceptance weight.
  // Rounding 0.5 * 1 → 1 is acceptable: uncertain single-draw leaves are
  // included at full weight, matching the "hedge" intent.
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
  leave_rack_list_insert_rack(&ctx->current_leave, /*exchanged=*/NULL,
                              (int)weighted, leave_equity,
                              inference_results_get_leave_rack_list(
                                  ctx->results));
}

// ── Evaluate + record ─────────────────────────────────────────────────────────

static void evaluate_and_record(SimmedInferCtx *ctx) {
  if (thread_control_get_status(ctx->thread_control) ==
      THREAD_CONTROL_STATUS_USER_INTERRUPT) {
    return;
  }
  double weight = sim_evaluate_candidate_leave(ctx);
  record_candidate_leave(ctx, weight);
}

// ── Exhaustive enumeration ─────────────────────────────────────────────────────
//
// Recursively enumerates all combinations of `tiles_to_place` tiles from
// bag_as_rack (multiset combinations, no duplicates). Mirrors the recursion
// in the static inference's iterate_through_all_possible_leaves.
static void exhaust_leaves(SimmedInferCtx *ctx, int tiles_to_place,
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

// ── Monte Carlo leave sampling ────────────────────────────────────────────────
//
// Samples leave_size tiles without replacement from bag_as_rack using a
// Fisher-Yates partial shuffle. Writes the result into ctx->current_leave.
static void sample_leave_mc(SimmedInferCtx *ctx) {
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
    uint64_t j = (uint64_t)i + prng_get_random_number(ctx->prng,
                                                       (uint64_t)(total - i));
    MachineLetter tmp = pool[i];
    pool[i] = pool[j];
    pool[j] = tmp;
    rack_add_letter(&ctx->current_leave, pool[i]);
  }
}

// ── Base game setup ───────────────────────────────────────────────────────────
//
// Prepares base_inner_game with:
//   - target player: target_played + target_known tiles
//   - nontarget player: nontarget_known tiles
//   - bag: all_tiles - target_played - target_known - nontarget_known
//
// Also populates bag_as_rack (the pool for candidate leave sampling).
// The nontarget's known tiles ARE drawn into base_inner_game so they are
// excluded from the leave pool. However, the inner sim's set_random_rack(NULL)
// will return them to the bag and draw a fresh random rack each iteration,
// ensuring the target player never has knowledge of the nontarget's rack.
static bool setup_base_inner_game(SimmedInferCtx *ctx,
                                  const SimmedInferenceArgs *args,
                                  ErrorStack *error_stack) {
  const InferenceArgs *base = args->base;

  ctx->base_inner_game = game_duplicate(base->game);

  // Start from a clean pool: return both racks to the bag.
  return_rack_to_bag(ctx->base_inner_game, 0);
  return_rack_to_bag(ctx->base_inner_game, 1);

  // Build combined known target tiles = target_played + target_known.
  rack_set_dist_size_and_reset(&ctx->target_known_combined, ctx->ld_size);
  rack_union(&ctx->target_known_combined, base->target_played_tiles);
  rack_union(&ctx->target_known_combined, base->target_known_rack);

  if (!draw_rack_from_bag(ctx->base_inner_game, ctx->target_index,
                          &ctx->target_known_combined)) {
    error_stack_push(
        error_stack, ERROR_STATUS_INFERENCE_TARGET_LETTERS_NOT_IN_BAG,
        string_duplicate(
            "simmed inference: failed to draw combined target tiles from bag"));
    return false;
  }

  if (!draw_rack_from_bag(ctx->base_inner_game, 1 - ctx->target_index,
                          base->nontarget_known_rack)) {
    // This mirrors the log_fatal in static inference for the same condition.
    log_fatal("simmed inference: failed to draw nontarget known rack from bag");
  }

  // bag_as_rack = tiles remaining after both known racks are drawn.
  // This is the pool from which candidate leaves will be sampled.
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

// ── Entry point ───────────────────────────────────────────────────────────────

void simmed_infer(const SimmedInferenceArgs *args, InferenceResults *results,
                  ErrorStack *error_stack) {
  const InferenceArgs *base = args->base;

  if (!args->observed_move) {
    error_stack_push(
        error_stack, ERROR_STATUS_INFERENCE_NO_TILES_PLAYED,
        string_duplicate("simmed_infer: no observed move provided"));
    return;
  }

  const int known_target_tiles =
      rack_get_total_letters(base->target_played_tiles) +
      rack_get_total_letters(base->target_known_rack);
  const int leave_size = RACK_SIZE - known_target_tiles;
  if (leave_size < 0) {
    error_stack_push(
        error_stack, ERROR_STATUS_INFERENCE_RACK_OVERFLOW,
        string_duplicate(
            "simmed_infer: more known target tiles than rack size"));
    return;
  }

  SimmedInferCtx ctx;
  memset(&ctx, 0, sizeof(ctx));

  ctx.target_index = base->target_index;
  ctx.ld_size = ld_get_size(game_get_ld(base->game));
  ctx.leave_size = leave_size;
  ctx.num_candidate_plays = args->num_candidate_plays;
  ctx.num_inner_sim_plies = args->num_inner_sim_plies;
  ctx.probe_iterations = args->probe_iterations;
  ctx.full_iterations = args->full_iterations;
  ctx.time_budget_s = args->time_budget_s;
  ctx.sim_equity_margin =
      (args->sim_equity_margin > 0.0) ? args->sim_equity_margin : 3.0;
  ctx.thread_control = base->thread_control;
  ctx.win_pcts = args->win_pcts;
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

  inference_results_reset(results, base->move_capacity, ctx.ld_size);

  // RACK_SIZE extra slots: for exchanges, we add one best-exchange arm per
  // possible leave size (0..RACK_SIZE-1), i.e. exchange counts 1..RACK_SIZE.
  ctx.inner_move_list = move_list_create(args->num_candidate_plays + RACK_SIZE);
  ctx.inner_sim_results = sim_results_create(0.0);
  ctx.inner_sim_ctx = NULL;
  ctx.inner_error_stack = error_stack_create();
  ctx.inner_game = game_duplicate(ctx.base_inner_game);
  ctx.prng = prng_create(42);

  if (leave_size == 0) {
    // Bingo or full exchange: leave is empty, only one candidate.
    rack_reset(&ctx.current_leave);
    evaluate_and_record(&ctx);
  } else if (leave_size <= SIMMED_INFER_EXHAUSTIVE_MAX) {
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

  inference_results_finalize(
      base->target_played_tiles,
      base->target_known_rack,
      &ctx.bag_as_rack, results,
      base->target_score, base->target_num_exch, base->equity_margin,
      thread_control_get_status(ctx.thread_control) ==
          THREAD_CONTROL_STATUS_USER_INTERRUPT);

  prng_destroy(ctx.prng);
  error_stack_destroy(ctx.inner_error_stack);
  sim_ctx_destroy(ctx.inner_sim_ctx);
  sim_results_destroy(ctx.inner_sim_results);
  move_list_destroy(ctx.inner_move_list);
  game_destroy(ctx.inner_game);
  game_destroy(ctx.base_inner_game);
}
