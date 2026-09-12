// Unit tests for simmed_inference.c.
//
// Regression tests for bugs found during SIM4 vs SIMMEDINF4 benchmarking.

#include "../src/def/config_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/board.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/inference_args.h"
#include "../src/ent/inference_results.h"
#include "../src/ent/klv.h"
#include "../src/ent/leave_rack.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/stats.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/validated_move.h"
#include "../src/ent/win_pct.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/inference.h"
#include "../src/impl/simmed_inference.h"
#include "../src/str/move_string.h"
#include "../src/str/rack_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_constants.h"
#include "test_util.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Call simmed_infer for a given exchange count from the OPENING_CGP game state
// (player 0 has ABCDEFG). Verifies no crash and no error on the error stack.
static void assert_simmedinf_exchange_no_crash(int exch_count,
                                               int num_threads) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1 -threads 4");
  load_and_exec_config_or_die(config, "cgp " OPENING_CGP);

  ErrorStack *error_stack = error_stack_create();
  WinPct *win_pcts = win_pct_create(config_get_data_paths(config),
                                    DEFAULT_WIN_PCT, error_stack);
  assert(error_stack_is_empty(error_stack));

  const Game *game = config_get_game(config);
  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);

  Rack target_played_tiles;
  Rack target_known_rack;
  Rack nontarget_known_rack;
  rack_set_dist_size_and_reset(&target_played_tiles, ld_size);
  rack_set_dist_size_and_reset(&target_known_rack, ld_size);
  rack_set_dist_size_and_reset(&nontarget_known_rack, ld_size);

  // Player 1 (nontarget) knows their own rack.
  rack_copy(&nontarget_known_rack, player_get_rack(game_get_player(game, 1)));

  // Build a minimal exchange move: only the count matters for inference
  // matching; specific tiles are irrelevant for exchange inference.
  Move observed_move;
  memset(&observed_move, 0, sizeof(observed_move));
  move_set_type(&observed_move, GAME_EVENT_EXCHANGE);
  move_set_tiles_played(&observed_move, exch_count);
  move_set_score(&observed_move, int_to_equity(0));

  InferenceArgs base_args;
  infer_args_fill(&base_args, /*num_plays=*/5, int_to_equity(0), NULL, game,
                  num_threads, /*parent_worker_thread_index=*/0,
                  /*print_interval=*/0, config_get_thread_control(config),
                  /*use_game_history=*/false,
                  /*use_inference_cutoff_optimization=*/true,
                  /*target_index=*/0, /*target_score=*/int_to_equity(0),
                  exch_count, &target_played_tiles, &target_known_rack,
                  &nontarget_known_rack);

  InferenceResults *inference_results = inference_results_create(NULL);

  SimmedInferenceArgs si_args = {
      .base = &base_args,
      .observed_move = &observed_move,
      .win_pcts = win_pcts,
      .num_candidate_plays = 5,
      .num_inner_sim_plies = 2,
      .probe_iterations = 20,
      .full_iterations = 40,
      .time_budget_s = 0.3,
      .sim_equity_margin = 3.0,
  };

  thread_control_set_status(config_get_thread_control(config),
                            THREAD_CONTROL_STATUS_STARTED);
  simmed_infer(&si_args, inference_results, error_stack);
  assert(error_stack_is_empty(error_stack));

  inference_results_destroy(inference_results);
  error_stack_destroy(error_stack);
  win_pct_destroy(win_pcts);
  config_destroy(config);
}

// Regression test: simmed_infer with a 5-tile exchange must not crash.
//
// Bug: when the generated candidate list had no exchange in the top-N
// (all tile placements), the code force-inserted observed_move_copy which
// had specific exchange tiles not present in the candidate leave, causing
// rack_take_letter underflow in execute_exchange_move.
//
// Fix: before trimming the generated list, scan beyond the top-N for any
// exchange of the right count and pre-save it, avoiding force-insertion
// of observed_move_copy with mismatched tiles.
void test_simmedinf_exchange_no_crash(void) {
  // 5-tile exchange: the case that originally triggered the crash.
  assert_simmedinf_exchange_no_crash(5, 4);
  // Also check 1-tile and 7-tile (full rack) exchanges.
  assert_simmedinf_exchange_no_crash(1, 4);
  assert_simmedinf_exchange_no_crash(7, 4);
}

// Regression tests for bugs found reviewing simmed_inference.c for the
// nested-sim/rack-inference oracle-eval investigation (2026-09-11): a Monte
// Carlo double-counting bug, a stale-equity bug on force-inserted moves, and
// an exchange-leave-splitting bug. See compute_leave_draw_weight's and
// record_candidate_leave's comments in simmed_inference.c for the full
// explanation of each.

// Bug 1: Monte Carlo-sampled leaves were reweighted by
// get_number_of_draws_for_rack (a combinatorial multiplier meant for
// exhaustive enumeration, where the pool already has the leave's tiles
// removed) even though sample_leave_mc draws directly from the undepleted
// pool -- already correctly weighting composition frequency by how often it
// samples each one. Astra's counterexample: pool {A:2, B:1}, a 1-tile leave,
// constant acceptance (weight=1.0 for every draw). The old code weighted A by
// choose(2+1,1)=3 and B by choose(1+1,1)=2, giving a posterior
// P(A) = (2/3*3)/((2/3*3)+(1/3*2)) = 3/4 instead of the correct 2/3 (the true
// fraction of physical tiles that are A). Fix: for Monte Carlo leaves, every
// draw gets the same flat weight regardless of which leave it is.
void test_simmedinf_mc_weight_is_composition_independent(void) {
  // A composition that would have looked "common" (raw_draws=3, matching
  // Astra's A) and one that would have looked "rare" (raw_draws=2, matching
  // Astra's B) must receive identical recorded weight under Monte Carlo
  // sampling at equal sim acceptance: raw_draws must be ignored entirely.
  const uint64_t w_common =
      compute_leave_draw_weight(/*is_monte_carlo=*/true, /*raw_draws=*/3, 1.0);
  const uint64_t w_rare =
      compute_leave_draw_weight(/*is_monte_carlo=*/true, /*raw_draws=*/2, 1.0);
  assert(w_common == w_rare);
  assert(w_common > 0);

  // Weight still scales the recorded count proportionally to sim acceptance.
  const uint64_t w_half = compute_leave_draw_weight(
      /*is_monte_carlo=*/true, /*raw_draws=*/999, 0.5);
  assert(w_half > 0);
  assert(w_half < w_common);

  // Exhaustive enumeration is unchanged: raw_draws still multiplies weight.
  assert(compute_leave_draw_weight(/*is_monte_carlo=*/false, /*raw_draws=*/5,
                                   1.0) == 5);
  assert(compute_leave_draw_weight(/*is_monte_carlo=*/false, /*raw_draws=*/5,
                                   0.0) == 0);
}

// Bug 3: a tile-placement move force-inserted into the candidate list
// (because it wasn't among the top num_candidate_plays by static equity)
// kept its equity from observed_move_copy, computed once under the rack the
// move was actually played from -- not under whichever candidate rack is
// currently being evaluated. Regression: for every evaluated candidate, the
// observed move's equity in the move list must equal score + the leave value
// of the CURRENT candidate rack after removing the move's tiles, whether or
// not the move needed force-insertion.
typedef struct {
  int target_index;
  int checked;
  int mismatches;
} EquityGroundTruthCtx;

static void equity_ground_truth_callback(const Rack *leave, const MoveList *ml,
                                         const SimResults *sr, int obs_idx,
                                         double gap, double weight,
                                         const Game *inner_game,
                                         void *user_data) {
  (void)leave;
  (void)sr;
  (void)gap;
  (void)weight;
  EquityGroundTruthCtx *ctx = (EquityGroundTruthCtx *)user_data;
  const Move *obs_move = move_list_get_move(ml, obs_idx);
  const KLV *klv =
      player_get_klv(game_get_player(inner_game, ctx->target_index));
  Rack rack_after_move;
  rack_copy(&rack_after_move,
            player_get_rack(game_get_player(inner_game, ctx->target_index)));
  const Equity expected_leave_value =
      get_leave_value_for_move(klv, obs_move, &rack_after_move);
  const Equity expected_equity =
      move_get_score(obs_move) + expected_leave_value;
  ctx->checked++;
  if (move_get_equity(obs_move) != expected_equity) {
    ctx->mismatches++;
  }
}

void test_simmedinf_force_inserted_equity(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1 -threads 4");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 "
              "QIAAAAA/ABCDEFG 0/0 0 -lex CSW21");

  ErrorStack *error_stack = error_stack_create();
  WinPct *win_pcts = win_pct_create(config_get_data_paths(config),
                                    DEFAULT_WIN_PCT, error_stack);
  assert(error_stack_is_empty(error_stack));

  const Game *game = config_get_game(config);
  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);

  ValidatedMoves *vms = validated_moves_create(
      game, /*player_index=*/0, "8G QI",
      /*allow_phonies=*/true, /*allow_playthrough=*/false, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(validated_moves_get_number_of_moves(vms) == 1);
  const Move *observed_move = validated_moves_get_move(vms, 0);

  Rack target_played_tiles;
  rack_set_dist_size_and_reset(&target_played_tiles, ld_size);
  const int tiles_length = move_get_tiles_length(observed_move);
  for (int i = 0; i < tiles_length; i++) {
    MachineLetter tile = move_get_tile(observed_move, i);
    if (tile != PLAYED_THROUGH_MARKER) {
      rack_add_letter(&target_played_tiles,
                      get_is_blanked(tile) ? BLANK_MACHINE_LETTER : tile);
    }
  }

  Rack target_known_rack;
  Rack nontarget_known_rack;
  rack_set_dist_size_and_reset(&target_known_rack, ld_size);
  rack_copy(&nontarget_known_rack, player_get_rack(game_get_player(game, 1)));

  InferenceArgs base_args;
  infer_args_fill(&base_args, /*leave_list_capacity=*/2000, int_to_equity(0),
                  NULL, game, /*num_threads=*/4,
                  /*parent_worker_thread_index=*/0, /*print_interval=*/0,
                  config_get_thread_control(config),
                  /*use_game_history=*/false,
                  /*use_inference_cutoff_optimization=*/true,
                  /*target_index=*/0,
                  /*target_score=*/move_get_score(observed_move),
                  /*target_num_exch=*/0, &target_played_tiles,
                  &target_known_rack, &nontarget_known_rack);

  InferenceResults *results = inference_results_create(NULL);
  EquityGroundTruthCtx cb_ctx = {.target_index = 0};

  // num_candidate_plays=1 keeps only the single best static move naturally,
  // so QI (a weak 2-tile play) is force-inserted for the large majority of
  // the 5-tile leaves this samples (Monte Carlo: leave_size=5).
  SimmedInferenceArgs si_args = {
      .base = &base_args,
      .observed_move = observed_move,
      .win_pcts = win_pcts,
      .num_candidate_plays = 1,
      .num_inner_sim_plies = 2,
      .probe_iterations = 20,
      .full_iterations = 40,
      .time_budget_s = 2.0,
      .sim_equity_margin = 3.0,
      .leave_callback = equity_ground_truth_callback,
      .leave_callback_data = &cb_ctx,
  };

  thread_control_set_status(config_get_thread_control(config),
                            THREAD_CONTROL_STATUS_STARTED);
  simmed_infer(&si_args, results, error_stack);
  assert(error_stack_is_empty(error_stack));

  assert(cb_ctx.checked > 0);
  assert(cb_ctx.mismatches == 0);

  inference_results_destroy(results);
  validated_moves_destroy(vms);
  win_pct_destroy(win_pcts);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

// Bug 4: exchange hypotheses were recorded under INFERENCE_TYPE_LEAVE using
// the full pre-exchange candidate rack, with exchanged always passed as NULL
// -- so what should have been "the retained leave" was actually still the
// full rack, never split by what the hypothesis exchanged. Regression: for a
// small, exhaustive-mode exchange observation, every recorded LeaveRack must
// have exactly observed_num_exch exchanged tiles, and its leave (retained)
// size plus its exchanged size must account for exactly the full candidate
// leave_size.
void test_simmedinf_exchange_leave_split(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1 -threads 4");
  load_and_exec_config_or_die(config, "cgp " OPENING_CGP);

  ErrorStack *error_stack = error_stack_create();
  WinPct *win_pcts = win_pct_create(config_get_data_paths(config),
                                    DEFAULT_WIN_PCT, error_stack);
  assert(error_stack_is_empty(error_stack));

  const Game *game = config_get_game(config);
  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);

  Rack target_played_tiles;
  Rack target_known_rack;
  Rack nontarget_known_rack;
  rack_set_dist_size_and_reset(&target_played_tiles, ld_size);
  rack_set_dist_size_and_reset(&nontarget_known_rack, ld_size);

  // 5 known target tiles -> leave_size = RACK_SIZE(7) - 5 = 2, small enough
  // for exhaustive enumeration (every candidate visited deterministically).
  const int leave_size = 2;
  rack_set_to_string(ld, &target_known_rack, "AEINR");
  rack_copy(&nontarget_known_rack, player_get_rack(game_get_player(game, 1)));

  const int exch_count = 1;
  Move observed_move;
  memset(&observed_move, 0, sizeof(observed_move));
  move_set_type(&observed_move, GAME_EVENT_EXCHANGE);
  move_set_tiles_played(&observed_move, exch_count);
  move_set_score(&observed_move, int_to_equity(0));

  InferenceArgs base_args;
  infer_args_fill(&base_args, /*leave_list_capacity=*/2000, int_to_equity(0),
                  NULL, game, /*num_threads=*/4,
                  /*parent_worker_thread_index=*/0, /*print_interval=*/0,
                  config_get_thread_control(config),
                  /*use_game_history=*/false,
                  /*use_inference_cutoff_optimization=*/true,
                  /*target_index=*/0, /*target_score=*/int_to_equity(0),
                  exch_count, &target_played_tiles, &target_known_rack,
                  &nontarget_known_rack);

  InferenceResults *results = inference_results_create(NULL);

  SimmedInferenceArgs si_args = {
      .base = &base_args,
      .observed_move = &observed_move,
      .win_pcts = win_pcts,
      .num_candidate_plays = 5,
      .num_inner_sim_plies = 2,
      .probe_iterations = 20,
      .full_iterations = 40,
      .time_budget_s = 0.3,
      .sim_equity_margin = 3.0,
  };

  thread_control_set_status(config_get_thread_control(config),
                            THREAD_CONTROL_STATUS_STARTED);
  simmed_infer(&si_args, results, error_stack);
  assert(error_stack_is_empty(error_stack));

  const LeaveRackList *lrl = inference_results_get_leave_rack_list(results);
  assert(lrl);
  const int n = leave_rack_list_get_count(lrl);
  assert(n > 0); // at least one candidate should have been accepted

  // The KLV-optimal exchange is chosen over the full candidate rack (known
  // tiles + this sampled leave), so it may prefer to exchange a known tile
  // instead of one from the leave -- in that case the recorded "exchanged"
  // is only the overlap (possibly empty) and is <= exch_count, not always
  // equal to it. What must always hold, regardless of overlap, is that
  // leave + exchanged reconstructs exactly the sampled leave_size: nothing
  // is lost or double-recorded relative to the pre-split current_leave.
  Rack leave;
  Rack exchanged;
  rack_set_dist_size_and_reset(&leave, ld_size);
  rack_set_dist_size_and_reset(&exchanged, ld_size);
  int entries_with_overlap = 0;
  for (int i = 0; i < n; i++) {
    const LeaveRack *entry = leave_rack_list_get_rack(lrl, i);
    leave_rack_get_leave(entry, &leave);
    leave_rack_get_exchanged(entry, &exchanged);
    assert(rack_get_total_letters(&exchanged) <= (uint16_t)exch_count);
    assert(rack_get_total_letters(&leave) +
               rack_get_total_letters(&exchanged) ==
           leave_size);
    if (rack_get_total_letters(&exchanged) > 0) {
      entries_with_overlap++;
    }
  }
  // The fixed behavior must differ from the old bug (leave == full
  // current_leave, exchanged always NULL/empty) for at least one candidate,
  // or this test would pass under the bug too.
  assert(entries_with_overlap > 0);

  inference_results_destroy(results);
  win_pct_destroy(win_pcts);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

void test_simmedinf(void) {
  test_simmedinf_exchange_no_crash();
  test_simmedinf_mc_weight_is_composition_independent();
  test_simmedinf_force_inserted_equity();
  test_simmedinf_exchange_leave_split();
}

// ── Leave callback: prints per-leave inner-sim detail to a FILE* ─────────────

// Top-K unique leaves tracked by accumulated posterior score (raw * weight).
// Streaming top-K: when full, evict the minimum if the new leaf's current
// contribution exceeds it.  Works well because MC sampling is proportional
// to the prior, so high-posterior leaves are visited frequently.
#define MAX_SUMMARY_LEAVES 100

typedef struct {
  Rack leave;
  double posterior; // accumulated raw * weight across all callbacks
  double klv;       // leave KLV equity (set on first insertion, deterministic)
} LeafSummaryEntry;

typedef struct {
  FILE *f;
  const LetterDistribution *ld;
  int ld_size;
  int target_index;
  // Only print per-leave detail when weight >= this threshold (0.0 = print
  // all).
  double min_weight_to_print;
  // Running accumulators for distributional stats (all leaves, no size limit).
  double total_raw;       // sum of raw combinatorial draw counts
  double total_posterior; // sum of raw * sim_weight
  double raw_sum_score, post_sum_score;
  double raw_sum_vow, post_sum_vow;
  double raw_sum_con, post_sum_con;
  double raw_sum_bl, post_sum_bl;
  // Per-letter expected count in leave (prior and posterior).
  double raw_sum_letter[MAX_ALPHABET_SIZE];
  double post_sum_letter[MAX_ALPHABET_SIZE];
  // Leave value (KLV) distributional shift.
  double raw_sum_klv, post_sum_klv;
  int total_leaves_seen;
  int static_eval_count; // leaves rejected by static eval (no inner sim)
  // Minimum-gap leave (most plausible rack even if all were rejected).
  double min_gap;
  Rack min_gap_leave;
  // Top-K unique leaves by accumulated posterior (for summary table).
  LeafSummaryEntry top_leaves[MAX_SUMMARY_LEAVES];
  int top_count;
  int top_min_idx; // index of the current minimum-posterior slot
} LeafCallbackCtx;

static void qintar_leave_callback(const Rack *leave, const MoveList *ml,
                                  const SimResults *sr, int obs_idx, double gap,
                                  double weight, const Game *inner_game,
                                  void *user_data) {
  LeafCallbackCtx *cb = (LeafCallbackCtx *)user_data;
  const LetterDistribution *ld = cb->ld;
  const int n = move_list_get_count(ml);
  const uint64_t iters = sim_results_get_iteration_count(sr);
  const Board *board = game_get_board(inner_game);

  // Build descending sort order by win% (sim) or static equity (no sim).
  int order[RACK_SIZE + 8]; // generous upper bound
  for (int i = 0; i < n; i++)
    order[i] = i;
  // Insertion sort descending.
  for (int i = 1; i < n; i++) {
    int key = order[i];
    double key_val;
    if (iters > 0) {
      key_val = stat_get_mean(
          simmed_play_get_win_pct_stat(sim_results_get_simmed_play(sr, key)));
    } else {
      key_val = equity_to_double(move_get_equity(move_list_get_move(ml, key)));
    }
    int j = i - 1;
    while (j >= 0) {
      double cmp;
      if (iters > 0) {
        cmp = stat_get_mean(simmed_play_get_win_pct_stat(
            sim_results_get_simmed_play(sr, order[j])));
      } else {
        cmp =
            equity_to_double(move_get_equity(move_list_get_move(ml, order[j])));
      }
      if (cmp > key_val)
        break;
      order[j + 1] = order[j];
      j--;
    }
    order[j + 1] = key;
  }

  // Header: leave rack + summary.
  // gap is in bogopoints when sim ran, equity points when statically rejected.
  StringBuilder *sb = string_builder_create();
  string_builder_add_rack(sb, leave, ld, /*blanks_first=*/false);
  const bool print_detail = (weight >= cb->min_weight_to_print);
  if (print_detail) {
    fprintf(cb->f,
            "\n=== Leave: %-6s  gap=%.2f(%s)  weight=%.3f  iters=%llu ===\n",
            string_builder_peek(sb), gap, (iters > 0) ? "bogo" : "pts", weight,
            (unsigned long long)iters);
  }

  // Compute the target's full rack (known_combined + current_leave) from
  // inner_game; we'll subtract each move's tiles to show the resulting leave.
  const Rack *target_rack =
      player_get_rack(game_get_player(inner_game, cb->target_index));

  // One line per candidate move, descending by win%:
  //   move, leave, win%, sim equity (or static equity), marked * for observed.
  Rack arm_leave;
  rack_set_dist_size_and_reset(&arm_leave, cb->ld_size);
  if (print_detail)
    for (int i = 0; i < n; i++) {
      const int idx = order[i];
      const Move *m = move_list_get_move(ml, idx);

      // Compute the leave for this arm.
      rack_copy(&arm_leave, target_rack);
      const game_event_t mtype = move_get_type(m);
      if (mtype == GAME_EVENT_TILE_PLACEMENT_MOVE) {
        const int tlen = move_get_tiles_length(m);
        for (int t = 0; t < tlen; t++) {
          const MachineLetter ml_tile = move_get_tile(m, t);
          if (ml_tile != PLAYED_THROUGH_MARKER) {
            rack_take_letter(&arm_leave, get_is_blanked(ml_tile)
                                             ? BLANK_MACHINE_LETTER
                                             : ml_tile);
          }
        }
      } else if (mtype == GAME_EVENT_EXCHANGE) {
        const int tlen = move_get_tiles_length(m);
        for (int t = 0; t < tlen; t++) {
          rack_take_letter(&arm_leave, move_get_tile(m, t));
        }
      }
      // For pass, arm_leave == full rack (nothing played).

      string_builder_clear(sb);
      string_builder_add_move(sb, board, m, ld, /*add_score=*/true);
      const char *move_str = string_builder_peek(sb);

      StringBuilder *leave_sb = string_builder_create();
      string_builder_add_rack(leave_sb, &arm_leave, ld, /*blanks_first=*/false);
      const char *leave_str = string_builder_peek(leave_sb);

      if (iters > 0) {
        const double sim_eq = stat_get_mean(
            simmed_play_get_equity_stat(sim_results_get_simmed_play(sr, idx)));
        const double wp_pct =
            100.0 * stat_get_mean(simmed_play_get_win_pct_stat(
                        sim_results_get_simmed_play(sr, idx)));
        fprintf(cb->f, "  %s %-32s  %-6s  wp=%5.1f%%  eq=%7.2f\n",
                (idx == obs_idx) ? "*" : " ", move_str, leave_str, wp_pct,
                sim_eq);
      } else {
        const double sim_eq = equity_to_double(move_get_equity(m));
        fprintf(cb->f, "  %s %-32s  %-6s  static=%7.2f\n",
                (idx == obs_idx) ? "*" : " ", move_str, leave_str, sim_eq);
      }
      string_builder_destroy(leave_sb);
    }
  string_builder_destroy(sb);

  // Track static-eval-only leaves (no inner sim ran).
  if (iters == 0)
    cb->static_eval_count++;

  // Track the minimum-gap leave (most plausible rack).
  if (cb->total_leaves_seen == 0 || gap < cb->min_gap) {
    cb->min_gap = gap;
    rack_copy(&cb->min_gap_leave, leave);
  }

  // Compute KLV for this leave (deterministic lookup, same result every call).
  const KLV *klv_data =
      player_get_klv(game_get_player(inner_game, cb->target_index));
  const double klv_val = equity_to_double(klv_get_leave_value(klv_data, leave));

  // Accumulate running stats for distributional shift (all leaves, no limit).
  {
    Rack pool;
    rack_set_dist_size_and_reset(&pool, cb->ld_size);
    int bag_counts[MAX_ALPHABET_SIZE];
    memset(bag_counts, 0, sizeof(bag_counts));
    bag_increment_unseen_count(game_get_bag(inner_game), bag_counts);
    for (int l = 0; l < cb->ld_size; l++) {
      rack_add_letters(&pool, l, bag_counts[l]);
    }
    rack_union(&pool, leave);
    const double raw = (double)get_number_of_draws_for_rack(&pool, leave);

    double score = 0.0, vow = 0.0, con = 0.0, bl = 0.0;
    for (int l = 0; l < cb->ld_size; l++) {
      const int cnt = (int)rack_get_letter(leave, l);
      if (cnt == 0)
        continue;
      if (l == BLANK_MACHINE_LETTER) {
        bl += cnt;
      } else if (ld_get_is_vowel(cb->ld, l)) {
        vow += cnt;
        score += cnt * equity_to_double(ld_get_score(cb->ld, l));
      } else {
        con += cnt;
        score += cnt * equity_to_double(ld_get_score(cb->ld, l));
      }
    }
    // Per-letter accumulation.
    for (int l = 0; l < cb->ld_size; l++) {
      const int cnt = (int)rack_get_letter(leave, l);
      if (cnt == 0)
        continue;
      cb->raw_sum_letter[l] += raw * cnt;
      cb->post_sum_letter[l] += raw * weight * cnt;
    }

    cb->total_raw += raw;
    cb->total_posterior += raw * weight;
    cb->raw_sum_klv += raw * klv_val;
    cb->post_sum_klv += raw * weight * klv_val;
    cb->raw_sum_score += raw * score;
    cb->post_sum_score += raw * weight * score;
    cb->raw_sum_vow += raw * vow;
    cb->post_sum_vow += raw * weight * vow;
    cb->raw_sum_con += raw * con;
    cb->post_sum_con += raw * weight * con;
    cb->raw_sum_bl += raw * bl;
    cb->post_sum_bl += raw * weight * bl;
    cb->total_leaves_seen++;

    // Update top-K unique leaves by accumulated posterior.
    const double ps = raw * weight;
    int found = -1;
    for (int j = 0; j < cb->top_count; j++) {
      bool match = true;
      for (int l = 0; l < cb->ld_size && match; l++) {
        if (rack_get_letter(&cb->top_leaves[j].leave, l) !=
            rack_get_letter(leave, l))
          match = false;
      }
      if (match) {
        found = j;
        break;
      }
    }
    if (found >= 0) {
      cb->top_leaves[found].posterior += ps;
      // Recompute min if we updated it.
      if (found == cb->top_min_idx) {
        cb->top_min_idx = 0;
        for (int j = 1; j < cb->top_count; j++) {
          if (cb->top_leaves[j].posterior <
              cb->top_leaves[cb->top_min_idx].posterior)
            cb->top_min_idx = j;
        }
      }
    } else if (cb->top_count < MAX_SUMMARY_LEAVES) {
      rack_copy(&cb->top_leaves[cb->top_count].leave, leave);
      cb->top_leaves[cb->top_count].posterior = ps;
      cb->top_leaves[cb->top_count].klv = klv_val;
      if (cb->top_count == 0 || ps < cb->top_leaves[cb->top_min_idx].posterior)
        cb->top_min_idx = cb->top_count;
      cb->top_count++;
    } else if (ps > cb->top_leaves[cb->top_min_idx].posterior) {
      // Evict the minimum slot.
      rack_copy(&cb->top_leaves[cb->top_min_idx].leave, leave);
      cb->top_leaves[cb->top_min_idx].posterior = ps;
      cb->top_leaves[cb->top_min_idx].klv = klv_val;
      // Recompute new minimum.
      cb->top_min_idx = 0;
      for (int j = 1; j < cb->top_count; j++) {
        if (cb->top_leaves[j].posterior <
            cb->top_leaves[cb->top_min_idx].posterior)
          cb->top_min_idx = j;
      }
    }
  }
}

// ── Shared summary writer ────────────────────────────────────────────────────
//
// Uses top_leaves (top-K unique leaves by accumulated posterior) for the
// per-leaf table with KLV computed at callback time.
// Uses running accumulators for the distributional shift.
static void write_simmedinf_summary(LeafCallbackCtx *cb,
                                    InferenceResults *results,
                                    const LetterDistribution *ld, int ld_size) {
  (void)results; // leave_rack_list no longer needed for KLV
  // Sort top_leaves descending by accumulated posterior.
  for (int i = 1; i < cb->top_count; i++) {
    LeafSummaryEntry key = cb->top_leaves[i];
    int j = i - 1;
    while (j >= 0 && cb->top_leaves[j].posterior < key.posterior) {
      cb->top_leaves[j + 1] = cb->top_leaves[j];
      j--;
    }
    cb->top_leaves[j + 1] = key;
  }

  FILE *f = cb->f;

  // Report the minimum-gap leave (most plausible rack regardless of rejection).
  if (cb->total_leaves_seen > 0) {
    StringBuilder *msb = string_builder_create();
    string_builder_add_rack(msb, &cb->min_gap_leave, ld,
                            /*blanks_first=*/false);
    fprintf(f, "\nMin-gap leave: %-10s  gap=%.2f\n", string_builder_peek(msb),
            cb->min_gap);
    string_builder_destroy(msb);
  }

  fprintf(f,
          "\n=== SUMMARY (top %d leaves by posterior, %d total / "
          "%d static-eval / %d simmed) ===\n",
          cb->top_count, cb->total_leaves_seen, cb->static_eval_count,
          cb->total_leaves_seen - cb->static_eval_count);
  fprintf(f, "%-10s  %7s  %6s\n", "Leave", "PostWt%", "KLV");
  fprintf(f, "%-10s  %7s  %6s\n", "-----", "-------", "---");

  StringBuilder *sb = string_builder_create();
  const double top_total =
      cb->total_posterior > 0.0 ? cb->total_posterior : 1.0;
  for (int i = 0; i < cb->top_count; i++) {
    const LeafSummaryEntry *e = &cb->top_leaves[i];
    const double post_pct = 100.0 * e->posterior / top_total;
    string_builder_clear(sb);
    string_builder_add_rack(sb, &e->leave, ld, /*blanks_first=*/false);
    fprintf(f, "%-10s  %6.2f%%  %6.2f\n", string_builder_peek(sb), post_pct,
            e->klv);
  }
  string_builder_destroy(sb);

  // Distributional shift: prior vs posterior from running accumulators.
  const double tr = cb->total_raw > 0.0 ? cb->total_raw : 1.0;
  const double tp = cb->total_posterior > 0.0 ? cb->total_posterior : 1.0;
  fprintf(f,
          "\n=== DISTRIBUTIONAL SHIFT (posterior − prior, %d leaves) ===\n"
          "%-12s  %8s  %8s  %8s\n"
          "%-12s  %8s  %8s  %8s\n"
          "%-12s  %8.3f  %8.3f  %+8.3f\n"
          "%-12s  %8.3f  %8.3f  %+8.3f\n"
          "%-12s  %8.3f  %8.3f  %+8.3f\n"
          "%-12s  %8.3f  %8.3f  %+8.3f\n"
          "%-12s  %8.3f  %8.3f  %+8.3f\n",
          cb->total_leaves_seen, "Attribute", "Prior", "Posterior", "Diff",
          "---------", "-----", "---------", "----", "LeaveValue",
          cb->raw_sum_klv / tr, cb->post_sum_klv / tp,
          cb->post_sum_klv / tp - cb->raw_sum_klv / tr, "TileScore",
          cb->raw_sum_score / tr, cb->post_sum_score / tp,
          cb->post_sum_score / tp - cb->raw_sum_score / tr, "Vowels",
          cb->raw_sum_vow / tr, cb->post_sum_vow / tp,
          cb->post_sum_vow / tp - cb->raw_sum_vow / tr, "Consonants",
          cb->raw_sum_con / tr, cb->post_sum_con / tp,
          cb->post_sum_con / tp - cb->raw_sum_con / tr, "Blanks",
          cb->raw_sum_bl / tr, cb->post_sum_bl / tp,
          cb->post_sum_bl / tp - cb->raw_sum_bl / tr);

  // Per-letter expected count: prior vs posterior, sorted by diff descending.
  // Collect letters with non-negligible prior or posterior.
  typedef struct {
    int ml;
    double prior;
    double post;
    double diff;
  } LetterStat;
  LetterStat lstats[MAX_ALPHABET_SIZE];
  int lstat_count = 0;
  for (int l = 0; l < ld_size; l++) {
    const double prior = cb->raw_sum_letter[l] / tr;
    const double post = cb->post_sum_letter[l] / tp;
    if (prior < 0.001 && post < 0.001)
      continue;
    lstats[lstat_count++] = (LetterStat){l, prior, post, post - prior};
  }
  // Sort by diff descending (biggest positive shift first).
  for (int i = 1; i < lstat_count; i++) {
    LetterStat key = lstats[i];
    int j = i - 1;
    while (j >= 0 && lstats[j].diff < key.diff) {
      lstats[j + 1] = lstats[j];
      j--;
    }
    lstats[j + 1] = key;
  }

  fprintf(f, "\n=== PER-LETTER EXPECTED COUNT IN LEAVE ===\n");
  fprintf(f, "%-6s  %7s  %7s  %7s\n", "Letter", "Prior", "Post", "Diff");
  fprintf(f, "%-6s  %7s  %7s  %7s\n", "------", "-----", "----", "----");
  StringBuilder *lsb = string_builder_create();
  Rack letter_rack;
  rack_set_dist_size_and_reset(&letter_rack, ld_size);
  for (int i = 0; i < lstat_count; i++) {
    const LetterStat *s = &lstats[i];
    rack_reset(&letter_rack);
    rack_add_letter(&letter_rack, s->ml);
    string_builder_clear(lsb);
    string_builder_add_rack(lsb, &letter_rack, ld, /*blanks_first=*/false);
    fprintf(f, "%-6s  %7.4f  %7.4f  %+7.4f\n", string_builder_peek(lsb),
            s->prior, s->post, s->diff);
  }
  string_builder_destroy(lsb);
}

// ── On-demand: inspect leave weights after opponent plays 8D QINTAR ──────────
//
// Pre-play state: empty board, player 0 on turn.
// Player 0 (target) rack: QINTARB (QINTAR + one unknown tile we're inferring).
// Player 1 (nontarget, us) rack: ABCDEFG.
// Observed move: 8D QINTAR (6-tile play → 1-tile leave, exhaustive inference).
// Results written to /tmp/qintar_inference.txt.
void test_qintar_simmedinf(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1 -threads 10");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 "
              "QINTARB/ABCDEFG 0/0 0 -lex CSW21");

  ErrorStack *error_stack = error_stack_create();
  WinPct *win_pcts = win_pct_create(config_get_data_paths(config),
                                    DEFAULT_WIN_PCT, error_stack);
  assert(error_stack_is_empty(error_stack));

  const Game *game = config_get_game(config);
  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);

  // Parse the observed move (player 0 plays QINTAR across from 8D).
  ValidatedMoves *vms = validated_moves_create(
      game, /*player_index=*/0, "8D QINTAR",
      /*allow_phonies=*/true, /*allow_playthrough=*/false, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(validated_moves_get_number_of_moves(vms) == 1);
  const Move *observed_move = validated_moves_get_move(vms, 0);

  // Extract the tiles played (QINTAR → {Q,I,N,T,A,R}); blanks become
  // BLANK_MACHINE_LETTER as in the static inference convention.
  Rack target_played_tiles;
  rack_set_dist_size_and_reset(&target_played_tiles, ld_size);
  const int tiles_length = move_get_tiles_length(observed_move);
  for (int i = 0; i < tiles_length; i++) {
    MachineLetter ml = move_get_tile(observed_move, i);
    if (ml != PLAYED_THROUGH_MARKER) {
      rack_add_letter(&target_played_tiles,
                      get_is_blanked(ml) ? BLANK_MACHINE_LETTER : ml);
    }
  }

  Rack target_known_rack;
  Rack nontarget_known_rack;
  rack_set_dist_size_and_reset(&target_known_rack, ld_size);
  // We (player 1) know our own rack.
  rack_copy(&nontarget_known_rack, player_get_rack(game_get_player(game, 1)));

  InferenceArgs base_args;
  infer_args_fill(&base_args, /*num_plays=*/15, int_to_equity(0), NULL, game,
                  /*num_threads=*/10, /*parent_worker_thread_index=*/0,
                  /*print_interval=*/0, config_get_thread_control(config),
                  /*use_game_history=*/false,
                  /*use_inference_cutoff_optimization=*/true,
                  /*target_index=*/0,
                  /*target_score=*/move_get_score(observed_move),
                  /*target_num_exch=*/0, &target_played_tiles,
                  &target_known_rack, &nontarget_known_rack);

  InferenceResults *results = inference_results_create(NULL);

  LeafCallbackCtx cb_ctx = {.f = fopen("/tmp/qintar_inference.txt", "w"),
                            .ld = ld,
                            .ld_size = ld_size,
                            .target_index = 0};
  assert(cb_ctx.f);
  fprintf(cb_ctx.f,
          "Simmed inference: opponent played 8D QINTAR (score: %d)\n"
          "Our rack (nontarget): ABCDEFG\n"
          "Leave size: 1 tile (exhaustive)\n"
          "* = observed move (QINTAR)\n",
          equity_to_int(move_get_score(observed_move)));

  SimmedInferenceArgs si_args = {
      .base = &base_args,
      .observed_move = observed_move,
      .win_pcts = win_pcts,
      .num_candidate_plays = 5,
      .num_inner_sim_plies = 2,
      .probe_iterations = 120,
      .full_iterations = 600,
      .time_budget_s = 30.0,
      .sim_equity_margin = 3.0,
      .leave_callback = qintar_leave_callback,
      .leave_callback_data = &cb_ctx,
  };

  thread_control_set_status(config_get_thread_control(config),
                            THREAD_CONTROL_STATUS_STARTED);
  simmed_infer(&si_args, results, error_stack);
  assert(error_stack_is_empty(error_stack));

  write_simmedinf_summary(&cb_ctx, results, ld, ld_size);
  fclose(cb_ctx.f);

  printf("Written to /tmp/qintar_inference.txt (%d leaves evaluated)\n",
         cb_ctx.total_leaves_seen);

  inference_results_destroy(results);
  validated_moves_destroy(vms);
  win_pct_destroy(win_pcts);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

// ── On-demand: inspect leave weights after opponent plays 8D DINGS
// ────────────
//
// Pre-play state: empty board, player 0 on turn.
// Player 0 (target) rack: DINGS + 2 unknown tiles (leave_size = 2, exhaustive).
// Player 1 (nontarget, us) rack: ABCDEFG.
// Observed move: 8D DINGS (5-tile play).
// Results written to /tmp/dings_inference.txt.
// Per-leave detail is printed only for leaves with weight >= 0.05.
void test_dings_simmedinf(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1 -threads 10");
  // Target rack in CGP: DINGS + arbitrary filler tiles (we override via
  // InferenceArgs; the exact filler doesn't affect inference).
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 "
              "DINGSAA/ABCDEFG 0/0 0 -lex CSW21");

  ErrorStack *error_stack = error_stack_create();
  WinPct *win_pcts = win_pct_create(config_get_data_paths(config),
                                    DEFAULT_WIN_PCT, error_stack);
  assert(error_stack_is_empty(error_stack));

  const Game *game = config_get_game(config);
  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);

  // Parse observed move: 8D DINGS.
  ValidatedMoves *vms = validated_moves_create(
      game, /*player_index=*/0, "8D DINGS",
      /*allow_phonies=*/true, /*allow_playthrough=*/false, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(validated_moves_get_number_of_moves(vms) == 1);
  const Move *observed_move = validated_moves_get_move(vms, 0);

  // Extract tiles played (D, I, N, G, S).
  Rack target_played_tiles;
  rack_set_dist_size_and_reset(&target_played_tiles, ld_size);
  const int tiles_length = move_get_tiles_length(observed_move);
  for (int i = 0; i < tiles_length; i++) {
    MachineLetter ml = move_get_tile(observed_move, i);
    if (ml != PLAYED_THROUGH_MARKER) {
      rack_add_letter(&target_played_tiles,
                      get_is_blanked(ml) ? BLANK_MACHINE_LETTER : ml);
    }
  }

  Rack target_known_rack;
  Rack nontarget_known_rack;
  rack_set_dist_size_and_reset(&target_known_rack, ld_size);
  rack_copy(&nontarget_known_rack, player_get_rack(game_get_player(game, 1)));

  InferenceArgs base_args;
  infer_args_fill(&base_args, /*num_plays=*/15, int_to_equity(0), NULL, game,
                  /*num_threads=*/10, /*parent_worker_thread_index=*/0,
                  /*print_interval=*/0, config_get_thread_control(config),
                  /*use_game_history=*/false,
                  /*use_inference_cutoff_optimization=*/true,
                  /*target_index=*/0,
                  /*target_score=*/move_get_score(observed_move),
                  /*target_num_exch=*/0, &target_played_tiles,
                  &target_known_rack, &nontarget_known_rack);

  InferenceResults *results = inference_results_create(NULL);

  LeafCallbackCtx cb_ctx = {.f = fopen("/tmp/dings_inference.txt", "w"),
                            .ld = ld,
                            .ld_size = ld_size,
                            .target_index = 0,
                            .min_weight_to_print = 0.05};
  assert(cb_ctx.f);
  fprintf(cb_ctx.f,
          "Simmed inference: opponent played 8D DINGS (score: %d)\n"
          "Our rack (nontarget): ABCDEFG\n"
          "Leave size: 2 tiles (exhaustive)\n"
          "* = observed move (DINGS)\n"
          "(Per-leave detail shown only for weight >= 0.05)\n",
          equity_to_int(move_get_score(observed_move)));

  SimmedInferenceArgs si_args = {
      .base = &base_args,
      .observed_move = observed_move,
      .win_pcts = win_pcts,
      .num_candidate_plays = 5,
      .num_inner_sim_plies = 2,
      .probe_iterations = 120,
      .full_iterations = 600,
      .time_budget_s = 60.0,
      .sim_equity_margin = 3.0,
      .leave_callback = qintar_leave_callback,
      .leave_callback_data = &cb_ctx,
  };

  thread_control_set_status(config_get_thread_control(config),
                            THREAD_CONTROL_STATUS_STARTED);
  simmed_infer(&si_args, results, error_stack);
  assert(error_stack_is_empty(error_stack));

  write_simmedinf_summary(&cb_ctx, results, ld, ld_size);
  fclose(cb_ctx.f);

  printf("Written to /tmp/dings_inference.txt (%d leaves evaluated, "
         "%d in summary)\n",
         cb_ctx.total_leaves_seen,
         (int)(inference_results_get_leave_rack_list(results)
                   ? leave_rack_list_get_count(
                         inference_results_get_leave_rack_list(results))
                   : 0));

  inference_results_destroy(results);
  validated_moves_destroy(vms);
  win_pct_destroy(win_pcts);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

// ── On-demand: inspect leave weights after opponent plays 8G QI ──────────────
//
// Pre-play state: empty board, player 0 on turn.
// Player 0 (target) rack: QI + 5 unknown tiles (leave_size = 5, MC mode).
// Player 1 (nontarget, us) rack: ABCDEFG.
// Observed move: 8G QI (2-tile play, score: 22).
// Results written to /tmp/qi_inference.txt.
// Per-leave detail is printed only for leaves with weight >= 0.10.
void test_qi_simmedinf(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1 -threads 10");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 "
              "QIAAAAA/ABCDEFG 0/0 0 -lex CSW21");

  ErrorStack *error_stack = error_stack_create();
  WinPct *win_pcts = win_pct_create(config_get_data_paths(config),
                                    DEFAULT_WIN_PCT, error_stack);
  assert(error_stack_is_empty(error_stack));

  const Game *game = config_get_game(config);
  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);

  // Parse observed move: 8G QI (horizontal).
  ValidatedMoves *vms = validated_moves_create(
      game, /*player_index=*/0, "8G QI",
      /*allow_phonies=*/true, /*allow_playthrough=*/false, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(validated_moves_get_number_of_moves(vms) == 1);
  const Move *observed_move = validated_moves_get_move(vms, 0);

  // Extract tiles played (Q, I).
  Rack target_played_tiles;
  rack_set_dist_size_and_reset(&target_played_tiles, ld_size);
  const int tiles_length = move_get_tiles_length(observed_move);
  for (int i = 0; i < tiles_length; i++) {
    MachineLetter ml = move_get_tile(observed_move, i);
    if (ml != PLAYED_THROUGH_MARKER) {
      rack_add_letter(&target_played_tiles,
                      get_is_blanked(ml) ? BLANK_MACHINE_LETTER : ml);
    }
  }

  Rack target_known_rack;
  Rack nontarget_known_rack;
  rack_set_dist_size_and_reset(&target_known_rack, ld_size);
  rack_copy(&nontarget_known_rack, player_get_rack(game_get_player(game, 1)));

  InferenceArgs base_args;
  infer_args_fill(&base_args, /*num_plays=*/15, int_to_equity(0), NULL, game,
                  /*num_threads=*/10, /*parent_worker_thread_index=*/0,
                  /*print_interval=*/0, config_get_thread_control(config),
                  /*use_game_history=*/false,
                  /*use_inference_cutoff_optimization=*/true,
                  /*target_index=*/0,
                  /*target_score=*/move_get_score(observed_move),
                  /*target_num_exch=*/0, &target_played_tiles,
                  &target_known_rack, &nontarget_known_rack);

  InferenceResults *results = inference_results_create(NULL);

  LeafCallbackCtx cb_ctx = {.f = fopen("/tmp/qi_inference.txt", "w"),
                            .ld = ld,
                            .ld_size = ld_size,
                            .target_index = 0,
                            .min_weight_to_print = 0.1};
  assert(cb_ctx.f);
  fprintf(cb_ctx.f,
          "Simmed inference: opponent played 8G QI (score: %d)\n"
          "Our rack (nontarget): ABCDEFG\n"
          "Leave size: 5 tiles (Monte Carlo, time-budgeted)\n"
          "* = observed move (QI)\n"
          "(Per-leave detail shown only for weight >= 0.10)\n",
          equity_to_int(move_get_score(observed_move)));

  SimmedInferenceArgs si_args = {
      .base = &base_args,
      .observed_move = observed_move,
      .win_pcts = win_pcts,
      .num_candidate_plays = 5,
      .num_inner_sim_plies = 2,
      .probe_iterations = 120,
      .full_iterations = 600,
      .time_budget_s = 120.0,
      .sim_equity_margin = 3.0,
      .leave_callback = qintar_leave_callback,
      .leave_callback_data = &cb_ctx,
  };

  thread_control_set_status(config_get_thread_control(config),
                            THREAD_CONTROL_STATUS_STARTED);
  simmed_infer(&si_args, results, error_stack);
  assert(error_stack_is_empty(error_stack));

  write_simmedinf_summary(&cb_ctx, results, ld, ld_size);
  fclose(cb_ctx.f);

  printf("Written to /tmp/qi_inference.txt (%d leaves sampled, "
         "%d in summary)\n",
         cb_ctx.total_leaves_seen,
         (int)(inference_results_get_leave_rack_list(results)
                   ? leave_rack_list_get_count(
                         inference_results_get_leave_rack_list(results))
                   : 0));

  inference_results_destroy(results);
  validated_moves_destroy(vms);
  win_pct_destroy(win_pcts);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

// ── On-demand: inspect leave weights after opponent plays 8H ES ──────────────
//
// Pre-play state: empty board, player 0 on turn.
// Player 0 (target) rack: ES + 5 unknown tiles (leave_size = 5, MC mode).
// Player 1 (nontarget, us) rack: ABCDEFG.
// Observed move: 8H ES (2-tile play, center square).
// Results written to /tmp/es_inference.txt.
// Per-leave detail is printed only for leaves with weight >= 0.10.
void test_es_simmedinf(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1 -threads 10");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 "
              "ESAAAAA/ABCDEFG 0/0 0 -lex CSW21");

  ErrorStack *error_stack = error_stack_create();
  WinPct *win_pcts = win_pct_create(config_get_data_paths(config),
                                    DEFAULT_WIN_PCT, error_stack);
  assert(error_stack_is_empty(error_stack));

  const Game *game = config_get_game(config);
  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);

  // Parse observed move: 8H ES (horizontal, center square).
  ValidatedMoves *vms = validated_moves_create(
      game, /*player_index=*/0, "8H ES",
      /*allow_phonies=*/true, /*allow_playthrough=*/false, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(validated_moves_get_number_of_moves(vms) == 1);
  const Move *observed_move = validated_moves_get_move(vms, 0);

  // Extract tiles played (E, S).
  Rack target_played_tiles;
  rack_set_dist_size_and_reset(&target_played_tiles, ld_size);
  const int tiles_length = move_get_tiles_length(observed_move);
  for (int i = 0; i < tiles_length; i++) {
    MachineLetter ml = move_get_tile(observed_move, i);
    if (ml != PLAYED_THROUGH_MARKER) {
      rack_add_letter(&target_played_tiles,
                      get_is_blanked(ml) ? BLANK_MACHINE_LETTER : ml);
    }
  }

  Rack target_known_rack;
  Rack nontarget_known_rack;
  rack_set_dist_size_and_reset(&target_known_rack, ld_size);
  rack_copy(&nontarget_known_rack, player_get_rack(game_get_player(game, 1)));

  InferenceArgs base_args;
  infer_args_fill(&base_args, /*num_plays=*/15, int_to_equity(0), NULL, game,
                  /*num_threads=*/10, /*parent_worker_thread_index=*/0,
                  /*print_interval=*/0, config_get_thread_control(config),
                  /*use_game_history=*/false,
                  /*use_inference_cutoff_optimization=*/true,
                  /*target_index=*/0,
                  /*target_score=*/move_get_score(observed_move),
                  /*target_num_exch=*/0, &target_played_tiles,
                  &target_known_rack, &nontarget_known_rack);

  InferenceResults *results = inference_results_create(NULL);

  LeafCallbackCtx cb_ctx = {.f = fopen("/tmp/es_inference.txt", "w"),
                            .ld = ld,
                            .ld_size = ld_size,
                            .target_index = 0,
                            .min_weight_to_print = 0.1};
  assert(cb_ctx.f);
  fprintf(cb_ctx.f,
          "Simmed inference: opponent played 8H ES (score: %d)\n"
          "Our rack (nontarget): ABCDEFG\n"
          "Leave size: 5 tiles (Monte Carlo, time-budgeted)\n"
          "* = observed move (ES)\n"
          "(Per-leave detail shown only for weight >= 0.10)\n",
          equity_to_int(move_get_score(observed_move)));

  SimmedInferenceArgs si_args = {
      .base = &base_args,
      .observed_move = observed_move,
      .win_pcts = win_pcts,
      .num_candidate_plays = 5,
      .num_inner_sim_plies = 2,
      .probe_iterations = 30,
      .full_iterations = 30,
      .time_budget_s = 30.0,
      .sim_equity_margin = 3.0,
      .leave_callback = qintar_leave_callback,
      .leave_callback_data = &cb_ctx,
  };

  thread_control_set_status(config_get_thread_control(config),
                            THREAD_CONTROL_STATUS_STARTED);
  simmed_infer(&si_args, results, error_stack);
  assert(error_stack_is_empty(error_stack));

  write_simmedinf_summary(&cb_ctx, results, ld, ld_size);
  fclose(cb_ctx.f);

  printf("Written to /tmp/es_inference.txt (%d leaves sampled, "
         "%d in summary)\n",
         cb_ctx.total_leaves_seen, cb_ctx.top_count);

  inference_results_destroy(results);
  validated_moves_destroy(vms);
  win_pct_destroy(win_pcts);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

// ── On-demand: inspect leave weights after opponent plays 8G EAU ─────────────
//
// Pre-play state: empty board, player 0 on turn.
// Player 0 (target) rack: EAU + 4 unknown tiles (leave_size = 4, MC mode).
// Player 1 (nontarget, us) rack: ABCDEFG.
// Observed move: 8G EAU (3-tile play).
// Results written to /tmp/eau_inference.txt.
void test_euoi_simmedinf(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1 -threads 10");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 "
              "EUOIAAA/ABCDEFG 0/0 0 -lex CSW21");

  ErrorStack *error_stack = error_stack_create();
  WinPct *win_pcts = win_pct_create(config_get_data_paths(config),
                                    DEFAULT_WIN_PCT, error_stack);
  assert(error_stack_is_empty(error_stack));

  const Game *game = config_get_game(config);
  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);

  ValidatedMoves *vms = validated_moves_create(
      game, /*player_index=*/0, "8G EUOI",
      /*allow_phonies=*/true, /*allow_playthrough=*/false, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(validated_moves_get_number_of_moves(vms) == 1);
  const Move *observed_move = validated_moves_get_move(vms, 0);

  Rack target_played_tiles;
  rack_set_dist_size_and_reset(&target_played_tiles, ld_size);
  const int tiles_length = move_get_tiles_length(observed_move);
  for (int i = 0; i < tiles_length; i++) {
    MachineLetter ml = move_get_tile(observed_move, i);
    if (ml != PLAYED_THROUGH_MARKER) {
      rack_add_letter(&target_played_tiles,
                      get_is_blanked(ml) ? BLANK_MACHINE_LETTER : ml);
    }
  }

  Rack target_known_rack;
  Rack nontarget_known_rack;
  rack_set_dist_size_and_reset(&target_known_rack, ld_size);
  rack_copy(&nontarget_known_rack, player_get_rack(game_get_player(game, 1)));

  InferenceArgs base_args;
  infer_args_fill(&base_args, /*num_plays=*/15, int_to_equity(0), NULL, game,
                  /*num_threads=*/10, /*parent_worker_thread_index=*/0,
                  /*print_interval=*/0, config_get_thread_control(config),
                  /*use_game_history=*/false,
                  /*use_inference_cutoff_optimization=*/true,
                  /*target_index=*/0,
                  /*target_score=*/move_get_score(observed_move),
                  /*target_num_exch=*/0, &target_played_tiles,
                  &target_known_rack, &nontarget_known_rack);

  InferenceResults *results = inference_results_create(NULL);

  LeafCallbackCtx cb_ctx = {.f = fopen("/tmp/euoi_inference.txt", "w"),
                            .ld = ld,
                            .ld_size = ld_size,
                            .target_index = 0,
                            .min_weight_to_print = 0.1};
  assert(cb_ctx.f);
  fprintf(cb_ctx.f,
          "Simmed inference: opponent played 8G EUOI (score: %d)\n"
          "Our rack (nontarget): ABCDEFG\n"
          "Leave size: 3 tiles (Monte Carlo, time-budgeted)\n"
          "* = observed move (EUOI)\n"
          "(Per-leave detail shown only for weight >= 0.10)\n",
          equity_to_int(move_get_score(observed_move)));

  SimmedInferenceArgs si_args = {
      .base = &base_args,
      .observed_move = observed_move,
      .win_pcts = win_pcts,
      .num_candidate_plays = 5,
      .num_inner_sim_plies = 2,
      .probe_iterations = 30,
      .full_iterations = 30,
      .time_budget_s = 30.0,
      .sim_equity_margin = 3.0,
      .leave_callback = qintar_leave_callback,
      .leave_callback_data = &cb_ctx,
  };

  thread_control_set_status(config_get_thread_control(config),
                            THREAD_CONTROL_STATUS_STARTED);
  simmed_infer(&si_args, results, error_stack);
  assert(error_stack_is_empty(error_stack));

  write_simmedinf_summary(&cb_ctx, results, ld, ld_size);
  fclose(cb_ctx.f);

  printf("Written to /tmp/euoi_inference.txt (%d leaves sampled, "
         "%d in summary)\n",
         cb_ctx.total_leaves_seen, cb_ctx.top_count);

  inference_results_destroy(results);
  validated_moves_destroy(vms);
  win_pct_destroy(win_pcts);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

void test_eau_simmedinf(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1 -threads 10");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 "
              "EAUAAAA/ABCDEFG 0/0 0 -lex CSW21");

  ErrorStack *error_stack = error_stack_create();
  WinPct *win_pcts = win_pct_create(config_get_data_paths(config),
                                    DEFAULT_WIN_PCT, error_stack);
  assert(error_stack_is_empty(error_stack));

  const Game *game = config_get_game(config);
  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);

  ValidatedMoves *vms = validated_moves_create(
      game, /*player_index=*/0, "8G EAU",
      /*allow_phonies=*/true, /*allow_playthrough=*/false, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(validated_moves_get_number_of_moves(vms) == 1);
  const Move *observed_move = validated_moves_get_move(vms, 0);

  Rack target_played_tiles;
  rack_set_dist_size_and_reset(&target_played_tiles, ld_size);
  const int tiles_length = move_get_tiles_length(observed_move);
  for (int i = 0; i < tiles_length; i++) {
    MachineLetter ml = move_get_tile(observed_move, i);
    if (ml != PLAYED_THROUGH_MARKER) {
      rack_add_letter(&target_played_tiles,
                      get_is_blanked(ml) ? BLANK_MACHINE_LETTER : ml);
    }
  }

  Rack target_known_rack;
  Rack nontarget_known_rack;
  rack_set_dist_size_and_reset(&target_known_rack, ld_size);
  rack_copy(&nontarget_known_rack, player_get_rack(game_get_player(game, 1)));

  InferenceArgs base_args;
  infer_args_fill(&base_args, /*num_plays=*/15, int_to_equity(0), NULL, game,
                  /*num_threads=*/10, /*parent_worker_thread_index=*/0,
                  /*print_interval=*/0, config_get_thread_control(config),
                  /*use_game_history=*/false,
                  /*use_inference_cutoff_optimization=*/true,
                  /*target_index=*/0,
                  /*target_score=*/move_get_score(observed_move),
                  /*target_num_exch=*/0, &target_played_tiles,
                  &target_known_rack, &nontarget_known_rack);

  InferenceResults *results = inference_results_create(NULL);

  LeafCallbackCtx cb_ctx = {.f = fopen("/tmp/eau_inference.txt", "w"),
                            .ld = ld,
                            .ld_size = ld_size,
                            .target_index = 0,
                            .min_weight_to_print = 0.1};
  assert(cb_ctx.f);
  fprintf(cb_ctx.f,
          "Simmed inference: opponent played 8G EAU (score: %d)\n"
          "Our rack (nontarget): ABCDEFG\n"
          "Leave size: 4 tiles (Monte Carlo, time-budgeted)\n"
          "* = observed move (EAU)\n"
          "(Per-leave detail shown only for weight >= 0.10)\n",
          equity_to_int(move_get_score(observed_move)));

  SimmedInferenceArgs si_args = {
      .base = &base_args,
      .observed_move = observed_move,
      .win_pcts = win_pcts,
      .num_candidate_plays = 5,
      .num_inner_sim_plies = 2,
      .probe_iterations = 30,
      .full_iterations = 30,
      .time_budget_s = 30.0,
      .sim_equity_margin = 3.0,
      .leave_callback = qintar_leave_callback,
      .leave_callback_data = &cb_ctx,
  };

  thread_control_set_status(config_get_thread_control(config),
                            THREAD_CONTROL_STATUS_STARTED);
  simmed_infer(&si_args, results, error_stack);
  assert(error_stack_is_empty(error_stack));

  write_simmedinf_summary(&cb_ctx, results, ld, ld_size);
  fclose(cb_ctx.f);

  printf("Written to /tmp/eau_inference.txt (%d leaves sampled, "
         "%d in summary)\n",
         cb_ctx.total_leaves_seen, cb_ctx.top_count);

  inference_results_destroy(results);
  validated_moves_destroy(vms);
  win_pct_destroy(win_pcts);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

// Pre-play state: empty board, player 0 on turn.
// Player 0 (target) rack: GUV + 4 unknown tiles (leave_size = 4, MC mode).
// Player 1 (nontarget, us) rack: ABCDEFG.
// Observed move: 8G GUV (3-tile play).
// Results written to /tmp/guv_inference.txt.
void test_guvy_simmedinf(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1 -threads 10");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 "
              "GUVAAAA/ABCDEFG 0/0 0 -lex CSW21");

  ErrorStack *error_stack = error_stack_create();
  WinPct *win_pcts = win_pct_create(config_get_data_paths(config),
                                    DEFAULT_WIN_PCT, error_stack);
  assert(error_stack_is_empty(error_stack));

  const Game *game = config_get_game(config);
  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);

  ValidatedMoves *vms = validated_moves_create(
      game, /*player_index=*/0, "8G GUV",
      /*allow_phonies=*/true, /*allow_playthrough=*/false, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(validated_moves_get_number_of_moves(vms) == 1);
  const Move *observed_move = validated_moves_get_move(vms, 0);

  Rack target_played_tiles;
  rack_set_dist_size_and_reset(&target_played_tiles, ld_size);
  const int tiles_length = move_get_tiles_length(observed_move);
  for (int i = 0; i < tiles_length; i++) {
    MachineLetter ml = move_get_tile(observed_move, i);
    if (ml != PLAYED_THROUGH_MARKER) {
      rack_add_letter(&target_played_tiles,
                      get_is_blanked(ml) ? BLANK_MACHINE_LETTER : ml);
    }
  }

  Rack target_known_rack;
  Rack nontarget_known_rack;
  rack_set_dist_size_and_reset(&target_known_rack, ld_size);
  rack_copy(&nontarget_known_rack, player_get_rack(game_get_player(game, 1)));

  InferenceArgs base_args;
  infer_args_fill(&base_args, /*num_plays=*/15, int_to_equity(0), NULL, game,
                  /*num_threads=*/10, /*parent_worker_thread_index=*/0,
                  /*print_interval=*/0, config_get_thread_control(config),
                  /*use_game_history=*/false,
                  /*use_inference_cutoff_optimization=*/true,
                  /*target_index=*/0,
                  /*target_score=*/move_get_score(observed_move),
                  /*target_num_exch=*/0, &target_played_tiles,
                  &target_known_rack, &nontarget_known_rack);

  InferenceResults *results = inference_results_create(NULL);

  LeafCallbackCtx cb_ctx = {.f = fopen("/tmp/guv_inference.txt", "w"),
                            .ld = ld,
                            .ld_size = ld_size,
                            .target_index = 0,
                            .min_weight_to_print = 0.1};
  assert(cb_ctx.f);
  fprintf(cb_ctx.f,
          "Simmed inference: opponent played 8G GUV (score: %d)\n"
          "Our rack (nontarget): ABCDEFG\n"
          "Leave size: 4 tiles (Monte Carlo, time-budgeted)\n"
          "* = observed move (GUV)\n"
          "(Per-leave detail shown only for weight >= 0.10)\n",
          equity_to_int(move_get_score(observed_move)));

  SimmedInferenceArgs si_args = {
      .base = &base_args,
      .observed_move = observed_move,
      .win_pcts = win_pcts,
      .num_candidate_plays = 5,
      .num_inner_sim_plies = 2,
      .probe_iterations = 30,
      .full_iterations = 30,
      .time_budget_s = 30.0,
      .sim_equity_margin = 3.0,
      .leave_callback = qintar_leave_callback,
      .leave_callback_data = &cb_ctx,
  };

  thread_control_set_status(config_get_thread_control(config),
                            THREAD_CONTROL_STATUS_STARTED);
  simmed_infer(&si_args, results, error_stack);
  assert(error_stack_is_empty(error_stack));

  write_simmedinf_summary(&cb_ctx, results, ld, ld_size);
  fclose(cb_ctx.f);

  printf("Written to /tmp/guv_inference.txt (%d leaves sampled, "
         "%d in summary)\n",
         cb_ctx.total_leaves_seen, cb_ctx.top_count);

  inference_results_destroy(results);
  validated_moves_destroy(vms);
  win_pct_destroy(win_pcts);
  error_stack_destroy(error_stack);
  config_destroy(config);
}
