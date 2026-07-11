// A/B benchmark for pre-endgame (PEG) inference. From a PEG position, play the
// game out with the inferring player using uniform PEG (arm A) vs
// inference-weighted PEG (arm B), an equal per-turn budget, and the d25 endgame
// once the bag empties. Logs per-turn wall time (inference + solve) to confirm
// neither arm overruns, and reports each arm's final spread.
//
// The inferring player's PEG turns weight peg_solve's scenarios by an inference
// of the opponent's leave from the opponent's previous move: simmed inference
// when the opponent moved with a bag >= 5, peg inference when <= 4 (they were in
// a PEG situation themselves). The opponent plays uniform PEG in both arms.
//
// Built incrementally: increment 3 adds the inference variant on top of the
// playout core (PEG + d25 endgame). Position generation and multi-position
// aggregation follow.

#include "../src/compat/ctime.h"
#include "../src/def/equity_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/peg_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/alias_method.h"
#include "../src/ent/endgame_results.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/inference_args.h"
#include "../src/ent/inference_results.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/sim_args.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/stats.h"
#include "../src/ent/win_pct.h"
#include "../src/impl/config.h"
#include "../src/impl/endgame.h"
#include "../src/impl/cgp.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/peg.h"
#include "../src/impl/inference.h"
#include "../src/impl/peg_inference.h"
#include "../src/impl/simmed_inference.h"
#include "../src/impl/simmer.h"
#include "../src/str/move_string.h"
#include "../src/str/rack_string.h"
#include "../src/compat/memory_info.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include <math.h>
#include "test_constants.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// A real 4-in-bag CSW24 pre-endgame: player 0 (ACEINOP) on turn, bag = 4.
#define PEG_BENCH_4BAG_CGP                                                    \
  "cgp 3V3W6L/1BEATY1U5GI/2XU3S4FEN/3TA2H4LOY/2GEN1DUCAT1AD1/2O1I1I2WRITE1/"  \
  "2V1M1ZOAEA4/3JAGER2DRILL/2BOtONE5O1/1FERER7Q1/4S8U1/12NaM/12ATE/13ST/14H " \
  "ACEINOP/DEIINOS 361/397 0 -lex CSW24"

#define PEG_BENCH_TURN_BUDGET_S 3.0
#define PEG_BENCH_MAX_CANDIDATES 20
#define PEG_BENCH_ENDGAME_PLIES 25
#define PEG_BENCH_INFERENCE_SAMPLES 200
// Larger sample counts cut the MC noise in arm B's per-candidate win% estimate
// (SE ~ sqrt(0.25/N)), so its move choice is driven by the inferred weighting
// rather than sampling noise.
// Fraction of the per-turn budget the inference step may use; the rest goes to
// the PEG solve so the whole turn stays within budget.
#define PEG_BENCH_INFER_BUDGET_FRAC 0.4
// Opponent's-turn bag >= this uses simmed inference; below uses peg inference.
#define PEG_BENCH_SIM_INFER_BAG 5
#define PEG_BENCH_INFER_CANDIDATES 7
// Per-class quotas of generated PEG positions the aggregation benchmark plays
// out A/B, and the cap on games scanned to fill them (peg-inf is rare).
#define PEG_BENCH_MAX_SIM 6
#define PEG_BENCH_MAX_PEG 4
#define PEG_BENCH_GAME_CAP 300

// Hardware threads each PEG solve / endgame / inference runs on (all cores, like
// a real peg run); set once per test from config_get_num_threads.
static int g_bench_num_threads = 1;
// Per-turn time budget (s) and arm-B MC sample count; env-overridable per test
// (PEGBENCH_BUDGET / PEGBENCH_SAMPLES) so a larger-budget run needs no rebuild.
static double g_bench_budget_s = PEG_BENCH_TURN_BUDGET_S;
static int g_bench_samples = PEG_BENCH_INFERENCE_SAMPLES;
// "Psychic" mode (PEGBENCH_PSYCHIC): arm B weights peg_solve by the opponent's
// TRUE leave instead of running inference -- the ceiling of leave inference and
// a check that leave results are applied correctly.
static bool g_bench_psychic = false;
// Greedy stage-0-only move selection (PEGBENCH_GREEDY, default OFF). Greedy
// values every scenario with a shallow greedy rollout, so the opponent prior
// can't translate into better moves -- off runs the full staged solve like real
// play (see peg_bench_config_solver). Default off is the fix.
static bool g_bench_greedy = false;
// Nested inner-peg recursion depth (PEGBENCH_NESTDEPTH, default
// PEG_NESTED_DEFAULT_DEPTH=1). A value >= the bag size nests the opponent's
// continuation all the way to the exact endgame (no greedy floor), so cranking
// this is the scoping lever for the win% overconfidence: if a deeper (more
// adversarial) continuation shrinks the peg-vs-ground-truth gap, the bias is the
// shallow/greedy continuation rather than winner's-curse selection.
static int g_bench_nest_depth = PEG_NESTED_DEFAULT_DEPTH;
// Inference-mode outer halving head (PEGBENCH_INFERTOPK, 0 = default {32,...}).
// The deep stages SAMPLE the opponent rack (inference_samples per candidate), so
// with the full 32-wide field the sampled endgame refinement can't finish in
// budget and the solve falls back to the greedy seed. Keeping only the top few
// candidates (the greedy seed still ranks the whole field) lets the inference
// solve reach deep stages. Schedule built as {k, k/2, ..., 2}.
static int g_bench_infer_topk = 0;
// Only capture "contested" positions where |score(p0) - score(p1)| <= this
// (PEGBENCH_CONTESTED; 0 = no filter). Blowouts are decided regardless of the
// opponent's rack, so inference can only matter in close positions.
static int g_bench_contested = 0;
// Calibration mode (PEGBENCH_CALIB = per-bucket quota, 0 = off): instead of the
// A/B ground truth, score the INFERENCE ITSELF against the known true leave --
// no solves, no playouts. For each position run the real inference and record
// the confidence statistic (top-leave mass share) alongside where the true
// leave landed (found / mass / rank). Positions are STRATIFIED by true-leave
// size k (0..7): each k bucket accepts up to the quota, then further k
// positions are skipped, so rare sizes (small plays => large leaves) get
// coverage instead of being swamped by the common 4-5s. The resulting
// accuracy-vs-confidence curve answers whether a confidence gate can make real
// inference useful: steep => gate viable; flat => the inference weights are
// miscalibrated and the generator is what needs fixing.
static int g_bench_calib = 0;
// Calibration slice filters: only accept positions whose true-leave size is in
// [PEGBENCH_CALIB_KMIN, PEGBENCH_CALIB_KMAX], and (PEGBENCH_CALIB_SIMONLY) only
// sim-inference positions (bag >= PEG_BENCH_SIM_INFER_BAG) -- the peg-inference
// regime currently returns an empty support, so a budget study on it is
// meaningless.
static int g_bench_calib_kmin = 0;
static int g_bench_calib_kmax = 7;
static bool g_bench_calib_simonly = false;
// Sim-inference per-leaf evaluation iterations (PEGBENCH_INF_PROBE/FULL,
// defaults 20/40): the second budget lever -- time_budget_s bounds how many
// leaves the MC loop samples (coverage), these bound how sharply each sampled
// leaf is weighted (ranking quality).
static int g_bench_inf_probe = 20;
static int g_bench_inf_full = 40;
// peg-inference (bag <= 4) tuning. The 0.1s default inner-solve budget starves
// every candidate-leave evaluation: the observed move's eval misses the
// deadline, its utility is unusable, the leave records weight 0, and the
// support comes out EMPTY (the 0-items regime the calibration found).
// PEGBENCH_PEGINF_SOLVE raises the per-eval budget; PEGBENCH_PEGINF_EXH raises
// exhaustive_max_leave so k <= that size enumerates ALL leaves (coverage)
// instead of MC-sampling a tiny fraction.
static double g_bench_peginf_solve_s = 0.1;
static int g_bench_peginf_exh = 1;
// Realistic-opponent generation (PEGBENCH_REALOPP, default off): instead of a
// static-equity player, generated games play SIM moves at bag >= 5 and uniform
// PEG_SOLVE moves at bag <= 4 once the bag reaches PEGBENCH_GEN_AUTHBAG
// (static-equity above that, where the inference never conditions -- it only
// sees the opponent's LAST move, at bag <= 11). This matches the opponent's
// actual policy to the model simmed/peg inference assumes, removing the
// model-mismatch that suppressed inference accuracy against a static player.
// PEGBENCH_GEN_SIMITERS sims per generated move (2-ply, matching the
// inference's inner sims); PEGBENCH_GEN_PEGBUDGET seconds per generated peg
// move.
static bool g_bench_realopp = false;
static int g_bench_gen_authbag = 12;
static int g_bench_gen_simiters = 100;
static double g_bench_gen_pegbudget = 5.0;
// Small-play gate (PEGBENCH_GATE, default off): arm B trusts the inference only
// on a sim-inference play whose estimated kept-leave size k_est is inside
// [PEGBENCH_GATE_KMIN, PEGBENCH_GATE_KMAX] (default 3..6 -- excludes bingos and
// big plays, where the inference is least accurate). The prior handed to
// peg_solve is the aggregated TOP-N leaves (PEGBENCH_GATE_TOPN, default 4,
// which fits the exact small-support enumeration path); otherwise uniform
// (B == A). See the gate block in the ground-truth loop.
static bool g_bench_gate = false;
static int g_bench_gate_topn = 4;
static int g_bench_gate_kmin = 3;
static int g_bench_gate_kmax = 6;
// Minimum OPPONENT-turn bag for the gate (PEGBENCH_GATE_OBMIN, default
// PEG_BENCH_SIM_INFER_BAG). The overnight slice showed the inference HURTS at
// opp-bag exactly 5 (-0.105, the sim's inner 2-ply rolls straight into the
// pre-endgame where its opponent model is least faithful) and HELPS at >= 6
// (+0.038): gate on 6 to drop the poisoned boundary cells.
static int g_bench_gate_obmin = PEG_BENCH_SIM_INFER_BAG;
// Maximum opponent-turn bag for the gate (PEGBENCH_GATE_OBMAX): with OBMIN this
// pins an exact opponent-bag slice, e.g. OBMIN=OBMAX=7 with k=1 selects
// "opponent played 6 tiles, one tile left in the bag at the mover's turn".
static int g_bench_gate_obmax = 99;
// Monte-Carlo samples for the ground-truth evaluation of a move (PEGBENCH_GT).
static int g_bench_gt_samples = 40;
// Tight per-endgame budget for the ground-truth MC playouts (PEGBENCH_GTENDGAME)
// -- kept small because it runs once per sample; small racks solve exactly well
// within it, hard stuck-tile ones cut off to a heuristic value (fine for an
// average, and identical for both compared moves).
static double g_bench_gt_endgame = 2.0;

// Read a positive integer environment override, or fall back to def.
static int peg_bench_env_int(const char *name, int def) {
  const char *v = getenv(name);
  if (v == NULL) {
    return def;
  }
  const int parsed = atoi(v);
  return parsed > 0 ? parsed : def;
}

// Read a boolean environment override (0 = false, non-zero = true, unset =
// def). Unlike peg_bench_env_int, "0" is a valid (false) value here.
static bool peg_bench_env_bool(const char *name, bool def) {
  const char *v = getenv(name);
  if (v == NULL) {
    return def;
  }
  return atoi(v) != 0;
}

// Read a positive double environment override, or fall back to def.
static double peg_bench_env_double(const char *name, double def) {
  const char *v = getenv(name);
  if (v == NULL) {
    return def;
  }
  const double parsed = atof(v);
  return parsed > 0.0 ? parsed : def;
}

static double peg_bench_now_s(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

// ── Inference setup (mirrors simmedinf_benchmark) ────────────────────────────

static void peg_bench_extract_played_tiles(const Move *move, int ld_size,
                                           Rack *played) {
  rack_set_dist_size_and_reset(played, ld_size);
  const int n = move_get_tiles_length(move);
  for (int i = 0; i < n; i++) {
    const MachineLetter ml = move_get_tile(move, i);
    if (ml != PLAYED_THROUGH_MARKER) {
      rack_add_letter(played, get_is_blanked(ml) ? BLANK_MACHINE_LETTER : ml);
    }
  }
}

typedef struct {
  Rack target_played_tiles;
  Rack target_known_rack;
  Rack nontarget_known_rack;
  InferenceArgs args;
} PegBenchInferSetup;

// Debug callback (PEGBENCH_PEGINF_DBG): one line per evaluated candidate leave,
// to diagnose why peg-inference records nothing. obs_idx = -1 means the
// observed move never entered the candidate field (e.g. an observed exchange
// with bag < RACK_SIZE builds no exchange arms); gap > 4*margin with weight 0
// is the static pre-filter cut; gap 0 with weight 0 is a failed/deadlined
// inner solve or unusable observed utility.
static void peg_bench_peginf_dbg(const Rack *candidate_leave,
                                 const MoveList *move_list, int obs_idx,
                                 double gap, double weight,
                                 const Game *inner_game, void *user_data) {
  (void)move_list;
  const LetterDistribution *ld = game_get_ld(inner_game);
  StringBuilder *sb = string_builder_create();
  string_builder_add_rack(sb, candidate_leave, ld, false);
  printf("      [peginf-dbg] leave=%-7s obs_idx=%d gap=%.4f weight=%.5f\n",
         string_builder_peek(sb), obs_idx, gap, weight);
  fflush(stdout);
  string_builder_destroy(sb);
  (void)user_data;
}

static void peg_bench_fill_infer_args(PegBenchInferSetup *setup,
                                      const Game *game_before_prev,
                                      const Move *prev_move,
                                      int prev_player_index,
                                      ThreadControl *thread_control) {
  const int ld_size = ld_get_size(game_get_ld(game_before_prev));
  rack_set_dist_size_and_reset(&setup->target_known_rack, ld_size);
  rack_set_dist_size_and_reset(&setup->nontarget_known_rack, ld_size);

  int target_num_exch = 0;
  if (move_get_type(prev_move) == GAME_EVENT_EXCHANGE) {
    target_num_exch = move_get_tiles_played(prev_move);
    rack_set_dist_size_and_reset(&setup->target_played_tiles, ld_size);
  } else {
    peg_bench_extract_played_tiles(prev_move, ld_size,
                                   &setup->target_played_tiles);
  }
  rack_copy(&setup->nontarget_known_rack,
            player_get_rack(
                game_get_player(game_before_prev, 1 - prev_player_index)));

  infer_args_fill(&setup->args, /*leave_list_capacity=*/PEG_BENCH_INFER_CANDIDATES,
                  int_to_equity(0), NULL, game_before_prev,
                  /*num_threads=*/g_bench_num_threads,
                  /*parent_worker_thread_index=*/0, /*print_interval=*/0,
                  thread_control, /*use_game_history=*/false,
                  /*use_inference_cutoff_optimization=*/false, prev_player_index,
                  move_get_score(prev_move), target_num_exch,
                  &setup->target_played_tiles, &setup->target_known_rack,
                  &setup->nontarget_known_rack);
}

// Infer the opponent's leave distribution from their previous move: simmed
// inference when they moved with a bag >= PEG_BENCH_SIM_INFER_BAG, peg inference
// when they were themselves in the PEG range. Returns true on success.
static bool peg_bench_run_inference(const Game *game_before_prev,
                                    const Move *prev_move,
                                    int prev_player_index, int prev_turn_bag,
                                    WinPct *win_pcts,
                                    ThreadControl *thread_control,
                                    InferenceResults *results, double budget_s,
                                    ErrorStack *error_stack) {
  PegBenchInferSetup setup;
  peg_bench_fill_infer_args(&setup, game_before_prev, prev_move,
                            prev_player_index, thread_control);
  thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED);
  // PEGBENCH_STATIC_INF: plain static-equity consistency inference (margin 0,
  // movegen-only -- no sims, no inner solves; works at ANY bag). This is the
  // MATCHED model for the benchmark opponent, which plays get_top_equity_move:
  // the true leave is accepted by construction, so found = 100% and the open
  // question becomes concentration (how few other leaves are consistent).
  if (getenv("PEGBENCH_STATIC_INF") != NULL) {
    infer_without_ctx(&setup.args, results, error_stack);
  } else if (prev_turn_bag >= PEG_BENCH_SIM_INFER_BAG) {
    const SimmedInferenceArgs si_args = {
        .base = &setup.args,
        .observed_move = prev_move,
        .win_pcts = win_pcts,
        .num_candidate_plays = PEG_BENCH_INFER_CANDIDATES,
        .num_inner_sim_plies = 2,
        .probe_iterations = g_bench_inf_probe,
        .full_iterations = g_bench_inf_full,
        .time_budget_s = budget_s,
        .sim_equity_margin = 3.0,
    };
    simmed_infer(&si_args, results, error_stack);
  } else {
    // Keep the inference inside its slice of the turn budget: bound each inner
    // PEG solve (peg_time_budget_s) and push leaves of size > 1 onto the
    // time-bounded Monte-Carlo path (only leaves of size <= exhaustive_max_leave
    // take peg_infer's unbounded exhaustive path).
    const PegInferenceArgs peg_args = {
        .base = &setup.args,
        .observed_move = prev_move,
        .num_candidate_plays = PEG_BENCH_INFER_CANDIDATES,
        .exhaustive_max_leave = g_bench_peginf_exh,
        .peg_time_budget_s = g_bench_peginf_solve_s,
        .time_budget_s = budget_s,
        .leave_callback =
            getenv("PEGBENCH_PEGINF_DBG") != NULL ? peg_bench_peginf_dbg : NULL,
    };
    peg_infer(&peg_args, results, error_stack);
  }
  if (!error_stack_is_empty(error_stack)) {
    error_stack_reset(error_stack);
    return false;
  }
  return true;
}

// ── Move selection ───────────────────────────────────────────────────────────

// Per-level nested-recursion candidate caps, matching the CLI default.
static const int PEG_BENCH_NESTED_CAPS[] = {8, 4, 2};

// Configure a PegArgs the way the real PEG solver runs in play (config.c): the
// full staged halving solve (unless greedy) with nested inner-peg lookahead on
// non-emptier leaves -- so scenarios are valued by exact endgames / recursive
// sub-pegs, not a shallow greedy rollout. A zero-init PegArgs leaves nested OFF,
// which made every scenario a weak greedy playout (the reason perfect opponent
// info couldn't move the needle).
static void peg_bench_config_solver(PegArgs *a) {
  a->greedy_seed_only = g_bench_greedy;
  a->nested_enabled = true;
  a->nested_max_depth = g_bench_nest_depth;
  a->nested_cand_caps = PEG_BENCH_NESTED_CAPS;
  a->nested_n_cand_caps =
      (int)(sizeof(PEG_BENCH_NESTED_CAPS) / sizeof(PEG_BENCH_NESTED_CAPS[0]));
  a->nested_stride = 0;
}

// Solve and play the endgame (bag empty), capped to budget_s. Copies the played
// move to *out_move. Returns true if a move was played.
static bool peg_bench_play_endgame(Game *game, ThreadControl *thread_control,
                                   double budget_s, int num_threads,
                                   Move *out_move, ErrorStack *error_stack) {
  EndgameArgs endgame_args = {0};
  endgame_args.thread_control = thread_control;
  endgame_args.game = game;
  endgame_args.plies = PEG_BENCH_ENDGAME_PLIES;
  endgame_args.tt_fraction_of_mem = 0.05;
  endgame_args.initial_small_move_arena_size =
      DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE;
  endgame_args.num_threads = num_threads;
  endgame_args.use_heuristics = true;
  endgame_args.forced_pass_bypass = true;
  endgame_args.enable_pv_display = true;
  endgame_args.num_top_moves = 1;
  endgame_args.seed = 0;
  endgame_args.soft_time_limit = budget_s;
  endgame_args.hard_time_limit = budget_s;
  endgame_args.external_deadline_ns =
      ctimer_monotonic_ns() + (int64_t)(budget_s * 1.0e9);

  if (getenv("PEGBENCH_DUMP_ENDGAME") != NULL) {
    char *cgp = game_get_cgp(game, /*write_player_on_turn_first=*/true);
    printf("    ENDGAME_CGP %s\n", cgp);
    fflush(stdout);
    free(cgp);
  }

  EndgameCtx *ctx = endgame_ctx_create();
  EndgameResults *results = endgame_results_create();
  thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED);
  endgame_solve(&ctx, &endgame_args, results, error_stack);

  bool played = false;
  if (error_stack_is_empty(error_stack)) {
    const PVLine *pv = endgame_results_get_pvline(results, ENDGAME_RESULT_BEST);
    if (pv != NULL && pv->num_moves > 0) {
      small_move_to_move(out_move, &pv->moves[0], game_get_board(game));
      play_move(out_move, game, NULL);
      played = true;
    }
  }
  endgame_results_destroy(results);
  endgame_ctx_destroy(ctx);
  return played;
}

// Choose and play one move: greedy PEG while the bag is in range (with the
// inference prior when non-NULL), the d25 endgame at bag 0. Copies the played
// move to *out_move; *elapsed_out is the solve wall time.
static void peg_bench_play_turn(Game *game, MoveList *move_list,
                                ThreadControl *thread_control, double budget_s,
                                const InferenceResults *prior, uint64_t seed,
                                Move *out_move, double *elapsed_out,
                                ErrorStack *error_stack) {
  const double t0 = peg_bench_now_s();
  const int bag = bag_get_letters(game_get_bag(game));
  bool played = false;
  if (bag == 0) {
    played = peg_bench_play_endgame(game, thread_control, budget_s,
                                    g_bench_num_threads, out_move, error_stack);
  } else if (bag >= PEG_MIN_BAG && bag <= PEG_MAX_BAG) {
    // Supply the candidate field explicitly (peg_solve's own root move gen is
    // unused by callers and returns only a pass); top-N by static equity.
    move_list_reset(move_list);
    const MoveGenArgs gen_args = {
        .game = game,
        .move_list = move_list,
        .move_record_type = MOVE_RECORD_ALL,
        .move_sort_type = MOVE_SORT_EQUITY,
        .override_kwg = NULL,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
        .tiles_played_bv = NULL,
    };
    generate_moves(&gen_args);
    move_list_sort_moves(move_list);
    const int total = move_list_get_count(move_list);
    const int n_cand =
        total < PEG_BENCH_MAX_CANDIDATES ? total : PEG_BENCH_MAX_CANDIDATES;
    const Move *cands[PEG_BENCH_MAX_CANDIDATES];
    for (int i = 0; i < n_cand; i++) {
      cands[i] = move_list_get_move(move_list, i);
    }
    if (n_cand > 0) {
      PegArgs peg_args = {0};
      peg_args.game = game;
      peg_args.thread_control = thread_control;
      peg_args.num_threads = g_bench_num_threads;
      peg_bench_config_solver(&peg_args);
      peg_args.time_budget_seconds = budget_s;
      peg_args.only_moves = cands;
      peg_args.n_only_moves = n_cand;
      peg_args.opp_leave_prior = prior;
      peg_args.inference_samples = prior ? g_bench_samples : 0;
      peg_args.inference_seed = seed;
      thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED);
      PegResult peg_result = {0};
      peg_solve(&peg_args, &peg_result, error_stack);
      if (error_stack_is_empty(error_stack) && peg_result.n_top_cands > 0 &&
          peg_result.best_win >= 0.0) {
        move_copy(out_move, &peg_result.best_move);
        play_move(out_move, game, NULL);
        played = true;
      }
      peg_result_destroy(&peg_result);
    }
  }
  if (!played) {
    const Move *move = get_top_equity_move(game, move_list);
    move_copy(out_move, move);
    play_move(out_move, game, NULL);
  }
  *elapsed_out = peg_bench_now_s() - t0;
}

// "Psychic" prior: pin the opponent-leave distribution to the opponent's ACTUAL
// kept tiles (leave = their pre-move rack minus the tiles they played) as a
// point mass. Feeding this true leave through peg_solve's normal weighting path
// -- which still samples the opponent's redraw from the unseen -- measures the
// ceiling of leave inference and validates that leave results are applied
// correctly (the same code path real inference uses, minus the estimation
// error). Returns a fresh InferenceResults the caller must destroy, or NULL.
static InferenceResults *peg_bench_psychic_prior(const Game *game_before_prev,
                                                 const Move *prev_move,
                                                 int prev_player) {
  const int ld_size = ld_get_size(game_get_ld(game_before_prev));
  const Rack *before =
      player_get_rack(game_get_player(game_before_prev, prev_player));
  Rack played;
  peg_bench_extract_played_tiles(prev_move, ld_size, &played);
  Rack leave;
  rack_set_dist_size_and_reset(&leave, ld_size);
  for (int ml = 0; ml < ld_size; ml++) {
    const int keep =
        (int)rack_get_letter(before, ml) - (int)rack_get_letter(&played, ml);
    for (int k = 0; k < keep; k++) {
      rack_add_letter(&leave, (MachineLetter)ml);
    }
  }
  AliasMethod *alias = alias_method_create();
  alias_method_add_rack(alias, &leave, 1);
  // PEGBENCH_PSYCHIC_MULTI: add the true leave a SECOND time so num_items == 2.
  // That routes the (still-perfect-info) psychic prior through the MULTI-support
  // paths (peg_enumerate_support / peg_collect_support) instead of the point-mass
  // pinned path -- a correctness probe: if psychic then still scores ~+0.024, the
  // support-enumeration machinery is correct and a bad REAL-inference result is
  // inference inaccuracy, not a machinery bug.
  if (getenv("PEGBENCH_PSYCHIC_MULTI") != NULL) {
    alias_method_add_rack(alias, &leave, 1);
  }
  if (!alias_method_generate_tables(alias)) {
    alias_method_destroy(alias);
    return NULL;
  }
  return inference_results_create(alias); // takes ownership of alias
}

// Dump one peg_solve candidate ranking (greedy full field) for debugging.
static void peg_bench_dump_ranking(const char *label, const PegResult *r,
                                   const Game *game) {
  const Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);
  StringBuilder *sb = string_builder_create();
  printf("    %s (%d cands, best_win=%.4f):\n", label, r->n_top_cands,
         r->best_win);
  for (int i = 0; i < r->n_top_cands; i++) {
    const PegRankedCand *c = &r->top_cands[i];
    string_builder_clear(sb);
    string_builder_add_move(sb, board, &c->move, ld, true);
    printf("      #%-2d %-16s win=%.4f spread=%+6.1f nscen=%d win=%lld tie=%lld "
           "wsum=%lld\n",
           i, string_builder_peek(sb), c->win_pct, c->mean_spread,
           c->n_scenarios, (long long)c->win_count, (long long)c->tie_count,
           (long long)c->weight_sum);
  }
  string_builder_destroy(sb);
}

// Run peg_solve on this position under both uniform and psychic priors (greedy,
// so top_cands is the full field) and dump both rankings. Exposes whether the
// psychic re-ranking is sane and its scenario weights are handled correctly.
static void peg_bench_debug_rankings(Game *game, MoveList *move_list,
                                     ThreadControl *thread_control,
                                     InferenceResults *prior, int samples,
                                     uint64_t seed) {
  move_list_reset(move_list);
  const MoveGenArgs gen_args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      .tiles_played_bv = NULL,
  };
  generate_moves(&gen_args);
  move_list_sort_moves(move_list);
  const int total = move_list_get_count(move_list);
  const int n_cand =
      total < PEG_BENCH_MAX_CANDIDATES ? total : PEG_BENCH_MAX_CANDIDATES;
  const Move *cands[PEG_BENCH_MAX_CANDIDATES];
  for (int i = 0; i < n_cand; i++) {
    cands[i] = move_list_get_move(move_list, i);
  }
  ErrorStack *es = error_stack_create();

  PegArgs base = {0};
  base.game = game;
  base.thread_control = thread_control;
  base.num_threads = g_bench_num_threads;
  base.greedy_seed_only = true;
  base.time_budget_seconds = g_bench_budget_s;
  base.only_moves = cands;
  base.n_only_moves = n_cand;

  thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED);
  PegResult ur = {0};
  peg_solve(&base, &ur, es);
  peg_bench_dump_ranking("UNIFORM", &ur, game);

  PegArgs pa = base;
  pa.opp_leave_prior = prior;
  pa.inference_samples = samples;
  pa.inference_seed = seed;
  thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED);
  PegResult pr = {0};
  peg_solve(&pa, &pr, es);
  peg_bench_dump_ranking("PSYCHIC", &pr, game);
  peg_result_destroy(&pr);

  // Determinism probe: re-run the identical psychic solve twice more and print
  // the #0 move each time. If the top move varies, peg_solve is non-deterministic
  // (multi-threaded scenario/endgame eval), so A/B deltas are partly noise.
  const Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);
  StringBuilder *sb = string_builder_create();
  for (int rep = 0; rep < 3; rep++) {
    thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED);
    PegResult rr = {0};
    peg_solve(&pa, &rr, es);
    string_builder_clear(sb);
    if (rr.n_top_cands > 0) {
      string_builder_add_move(sb, board, &rr.top_cands[0].move, ld, true);
    }
    printf("      [determinism] psychic rep %d: #0=%-16s win=%.4f spread=%+.1f\n",
           rep, string_builder_peek(sb),
           rr.n_top_cands > 0 ? rr.top_cands[0].win_pct : -1.0,
           rr.n_top_cands > 0 ? rr.top_cands[0].mean_spread : 0.0);
    peg_result_destroy(&rr);
  }
  string_builder_destroy(sb);

  fflush(stdout);
  peg_result_destroy(&ur);
  error_stack_destroy(es);
}

// ── Playout ──────────────────────────────────────────────────────────────────

// The opponent's previous-move context that lets inference fire on the very
// first turn of a replayed position (a generated PEG position starts with the
// inferring player already on turn, right after the opponent's move).
typedef struct {
  const Game *game_before_prev; // state before the opponent's prev move
  const Move *prev_move;        // opponent's prev move
  int prev_player;              // opponent index
  int prev_turn_bag;            // bag when the opponent moved (sim vs peg inf)
} PegBenchPrevCtx;

// Play the game to the end. The inferring player weights its PEG scenarios by
// an inference of the opponent's previous move when use_inference is set; the
// opponent always plays uniform PEG. When init is non-NULL the playout starts
// with that opponent-move context so inference can fire immediately. Returns the
// inferring player's final spread.
static int peg_bench_play_out(Game *game, MoveList *move_list,
                              ThreadControl *thread_control, WinPct *win_pcts,
                              int inferring_player, bool use_inference,
                              const PegBenchPrevCtx *init, bool verbose,
                              ErrorStack *error_stack) {
  Game *game_before_prev = game_duplicate(game);
  InferenceResults *inf_results = inference_results_create(NULL);
  Move prev_move;
  memset(&prev_move, 0, sizeof(prev_move));
  bool have_prev = false;
  int prev_player = -1;
  int prev_turn_bag = 0;
  if (init != NULL) {
    game_copy(game_before_prev, init->game_before_prev);
    move_copy(&prev_move, init->prev_move);
    have_prev = true;
    prev_player = init->prev_player;
    prev_turn_bag = init->prev_turn_bag;
  }

  int turn = 0;
  double worst = 0.0;
  while (!game_over(game)) {
    const int on_turn = game_get_player_on_turn_index(game);
    const int turn_bag = bag_get_letters(game_get_bag(game));
    Game *snapshot = game_duplicate(game);

    const InferenceResults *prior = NULL;
    InferenceResults *psychic_res = NULL; // per-turn; destroyed after the turn
    double infer_elapsed = 0.0;
    const bool prev_inferable =
        have_prev && (move_get_type(&prev_move) == GAME_EVENT_TILE_PLACEMENT_MOVE ||
                      move_get_type(&prev_move) == GAME_EVENT_EXCHANGE);
    if (use_inference && on_turn == inferring_player && prev_inferable &&
        turn_bag >= PEG_MIN_BAG && turn_bag <= PEG_MAX_BAG) {
      const double t0 = peg_bench_now_s();
      if (g_bench_psychic) {
        psychic_res =
            peg_bench_psychic_prior(game_before_prev, &prev_move, prev_player);
        prior = psychic_res;
        if (getenv("PEGBENCH_DEBUG_PSYCHIC") != NULL) {
          // Verify the computed "true leave" is a subset of the opponent's
          // ACTUAL current rack. If not, the leave models tiles the opponent
          // does not hold -- a bug, not a finding.
          const int ld_size = ld_get_size(game_get_ld(game));
          const Rack *before =
              player_get_rack(game_get_player(game_before_prev, prev_player));
          const Rack *opp_now =
              player_get_rack(game_get_player(game, prev_player));
          Rack played;
          peg_bench_extract_played_tiles(&prev_move, ld_size, &played);
          int subset_ok = 1;
          int leave_sz = 0;
          for (int ml = 0; ml < ld_size; ml++) {
            int keep = (int)rack_get_letter(before, ml) -
                       (int)rack_get_letter(&played, ml);
            if (keep < 0) {
              keep = 0;
            }
            leave_sz += keep;
            if (keep > (int)rack_get_letter(opp_now, ml)) {
              subset_ok = 0;
            }
          }
          printf("      [psy-dbg] turn=%d opp=p%d opp-bag=%d leave_size=%d "
                 "opp_now_size=%d leave_subset_of_opp_now=%s\n",
                 turn + 1, prev_player, prev_turn_bag, leave_sz,
                 rack_get_total_letters(opp_now), subset_ok ? "YES" : "NO(BUG)");
          fflush(stdout);
        }
        if (getenv("PEGBENCH_DEBUG_PEG") != NULL && psychic_res != NULL) {
          printf("    === peg ranking debug (turn %d, opp-bag=%d) ===\n",
                 turn + 1, prev_turn_bag);
          peg_bench_debug_rankings(game, move_list, thread_control, psychic_res,
                                   g_bench_samples, 42 + (uint64_t)turn);
        }
      } else {
        const double infer_budget =
            g_bench_budget_s * PEG_BENCH_INFER_BUDGET_FRAC;
        if (peg_bench_run_inference(game_before_prev, &prev_move, prev_player,
                                    prev_turn_bag, win_pcts, thread_control,
                                    inf_results, infer_budget, error_stack)) {
          prior = inf_results;
        }
      }
      infer_elapsed = peg_bench_now_s() - t0;
    }

    double solve_budget = g_bench_budget_s - infer_elapsed;
    if (solve_budget < 0.5) {
      solve_budget = 0.5;
    }
    Move played;
    double solve_elapsed = 0.0;
    peg_bench_play_turn(game, move_list, thread_control, solve_budget, prior,
                        /*seed=*/42 + (uint64_t)turn, &played, &solve_elapsed,
                        error_stack);

    const double turn_total = infer_elapsed + solve_elapsed;
    if (turn_total > worst) {
      worst = turn_total;
    }
    ++turn;
    if (verbose) {
      printf(
          "    turn %2d  p%d  bag=%d  infer=%.2fs solve=%.2fs total=%.2fs%s%s\n",
          turn, on_turn, turn_bag, infer_elapsed, solve_elapsed, turn_total,
          prior != NULL ? (g_bench_psychic ? "  [psy]" : "  [inf]") : "",
          turn_total > g_bench_budget_s * 1.5 ? "  *** OVER" : "");
    }
    if (psychic_res != NULL) {
      inference_results_destroy(psychic_res);
    }

    game_copy(game_before_prev, snapshot);
    game_destroy(snapshot);
    prev_move = played;
    have_prev = true;
    prev_player = on_turn;
    prev_turn_bag = turn_bag;

    if (!error_stack_is_empty(error_stack)) {
      error_stack_print_and_reset(error_stack);
      break;
    }
  }
  if (verbose) {
    printf("    (%d turns, worst turn %.2fs vs %.2fs budget)\n", turn, worst,
           g_bench_budget_s);
  }

  inference_results_destroy(inf_results);
  game_destroy(game_before_prev);
  const int p0_spread =
      equity_to_int(player_get_score(game_get_player(game, 0))) -
      equity_to_int(player_get_score(game_get_player(game, 1)));
  return inferring_player == 0 ? p0_spread : -p0_spread;
}

// ── Ground-truth move evaluation ─────────────────────────────────────────────

// The mover's chosen PEG move for this position: greedy peg_solve over the top
// candidates, weighted by `prior` when non-NULL. Copies it to *out; false if no
// move was produced.
static bool peg_bench_best_move(Game *game, MoveList *move_list,
                                ThreadControl *thread_control,
                                const InferenceResults *prior, int samples,
                                uint64_t seed, Move *out, double *out_peg_win,
                                double *out_peg_spread, int *out_peg_stage,
                                ErrorStack *error_stack) {
  move_list_reset(move_list);
  const MoveGenArgs gen_args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      .tiles_played_bv = NULL,
  };
  generate_moves(&gen_args);
  move_list_sort_moves(move_list);
  const int total = move_list_get_count(move_list);
  const int n_cand =
      total < PEG_BENCH_MAX_CANDIDATES ? total : PEG_BENCH_MAX_CANDIDATES;
  if (n_cand == 0) {
    return false;
  }
  const Move *cands[PEG_BENCH_MAX_CANDIDATES];
  for (int i = 0; i < n_cand; i++) {
    cands[i] = move_list_get_move(move_list, i);
  }
  PegArgs peg_args = {0};
  peg_args.game = game;
  peg_args.thread_control = thread_control;
  peg_args.num_threads = g_bench_num_threads;
  peg_bench_config_solver(&peg_args);
  peg_args.time_budget_seconds = g_bench_budget_s;
  peg_args.only_moves = cands;
  peg_args.n_only_moves = n_cand;
  peg_args.opp_leave_prior = prior;
  peg_args.inference_samples = prior ? samples : 0;
  peg_args.inference_seed = seed;
  // Inference mode: narrow the outer halving field so the costly sampled deep
  // stages fit the budget (the greedy seed still ranks all cands; only the top
  // g_bench_infer_topk carry into the depth ramp). Lives on the stack for the
  // duration of this peg_solve call.
  int infer_schedule[8];
  if (prior != NULL && g_bench_infer_topk > 1) {
    int n = 0;
    for (int k = g_bench_infer_topk; k > 2 && n < 7; k /= 2) {
      infer_schedule[n++] = k;
    }
    infer_schedule[n++] = 2;
    peg_args.stage_top_k = infer_schedule;
    peg_args.num_stages = n;
  }
  thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED);
  PegResult r = {0};
  peg_solve(&peg_args, &r, error_stack);
  bool ok = false;
  if (error_stack_is_empty(error_stack) && r.n_top_cands > 0 &&
      r.best_win >= 0.0) {
    move_copy(out, &r.best_move);
    // peg_solve's OWN estimate for the chosen move (top_cands[0] == best_move),
    // surfaced so callers can compare peg's win% to the ground truth
    // (calibration: does the solver believe it wins by more than reality?).
    if (out_peg_win != NULL) {
      *out_peg_win = r.top_cands[0].win_pct;
    }
    if (out_peg_spread != NULL) {
      *out_peg_spread = r.top_cands[0].mean_spread;
    }
    // Deepest stage reached (0 = greedy seed only; >0 = a halving stage at
    // fidelity 2+ completed). If this is 0, the fidelity knobs (nested depth,
    // emptier endgame plies) never fire -- the win% is a greedy playout.
    if (out_peg_stage != NULL) {
      *out_peg_stage = r.last_completed_stage;
    }
    ok = true;
  }
  // Optional: dump the full per-candidate outcomes table (win%/spread + W/T/wsum
  // scenario counts) for the last completed stage. Labels the arm by whether an
  // opponent-leave prior is in effect (PSYCHIC/inference vs UNIFORM).
  if (getenv("PEGBENCH_DUMPCANDS") != NULL && r.n_top_cands > 0) {
    printf("      [%s stage=%d] cand outcomes:\n",
           prior != NULL ? (g_bench_psychic ? "PSYCHIC" : "INFER") : "UNIFORM",
           r.last_completed_stage);
    peg_bench_dump_ranking(prior != NULL ? "  ranked" : "  ranked", &r, game);
  }
  peg_result_destroy(&r);
  return ok;
}

// Play out with static-equity moves (both players) until the bag empties, then
// the budget-capped d25 endgame; return the inferring player's final spread.
static int peg_bench_static_playout(Game *game, MoveList *move_list,
                                    ThreadControl *thread_control,
                                    int inferring_player, double endgame_budget,
                                    ErrorStack *error_stack) {
  while (!game_over(game)) {
    const int bag = bag_get_letters(game_get_bag(game));
    bool played = false;
    if (bag == 0) {
      Move mv;
      // Single-threaded => deterministic: the ground truth must not vary with
      // the parallel endgame's thread scheduling, or same-move samples diverge.
      played = peg_bench_play_endgame(game, thread_control, endgame_budget,
                                      /*num_threads=*/1, &mv, error_stack);
    }
    if (!played) {
      Move mv;
      move_copy(&mv, get_top_equity_move(game, move_list));
      play_move(&mv, game, NULL);
    }
    if (!error_stack_is_empty(error_stack)) {
      error_stack_print_and_reset(error_stack);
      break;
    }
  }
  const int p0 = equity_to_int(player_get_score(game_get_player(game, 0)));
  const int p1 = equity_to_int(player_get_score(game_get_player(game, 1)));
  return inferring_player == 0 ? p0 - p1 : p1 - p0;
}

// Ground-truth expected value of playing `move` from `pos`: for each of n
// samples, pin the opponent's rack to (actual_leave + a random completion) and
// reseed the bag (game_seed then set_random_rack, the sim model), play the move,
// then static-playout to the end. Averages the inferring player's spread and
// counts wins. The per-sample seeds are shared across the moves being compared
// so the draws are paired.
static void peg_bench_ground_truth(const Game *pos, const Move *move,
                                   const Rack *actual_leave, int opp_idx,
                                   int inferring_player, int n,
                                   uint64_t base_seed, MoveList *move_list,
                                   ThreadControl *thread_control,
                                   double endgame_budget, ErrorStack *error_stack,
                                   double *mean_spread_out, double *win_pts_out) {
  double sum_spread = 0.0;
  double win_pts = 0.0;
  for (int r = 0; r < n; r++) {
    Game *g = game_duplicate(pos);
    game_seed(g, base_seed + (uint64_t)r);
    set_random_rack(g, opp_idx, actual_leave);
    Move m;
    move_copy(&m, move);
    play_move(&m, g, NULL);
    const int sp = peg_bench_static_playout(g, move_list, thread_control,
                                            inferring_player, endgame_budget,
                                            error_stack);
    sum_spread += sp;
    win_pts += (sp > 0) ? 1.0 : (sp == 0 ? 0.5 : 0.0);
    game_destroy(g);
  }
  *mean_spread_out = sum_spread / n;
  *win_pts_out = win_pts / n;
}

// Compute the opponent's true leave (pre-move rack minus played tiles).
static void peg_bench_true_leave(const Game *game_before_prev,
                                 const Move *prev_move, int prev_player,
                                 Rack *leave) {
  const int ld_size = ld_get_size(game_get_ld(game_before_prev));
  const Rack *before =
      player_get_rack(game_get_player(game_before_prev, prev_player));
  Rack played;
  peg_bench_extract_played_tiles(prev_move, ld_size, &played);
  rack_set_dist_size_and_reset(leave, ld_size);
  for (int ml = 0; ml < ld_size; ml++) {
    const int keep =
        (int)rack_get_letter(before, ml) - (int)rack_get_letter(&played, ml);
    for (int k = 0; k < keep; k++) {
      rack_add_letter(leave, (MachineLetter)ml);
    }
  }
}

// Whether two tile-placement/pass moves are the same play (so their ground
// truth is identical -- no point sampling it).
static bool peg_bench_moves_equal(const Move *a, const Move *b) {
  if (move_get_type(a) != move_get_type(b)) {
    return false;
  }
  if (move_get_type(a) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
    return true; // passes/exchanges of the same type: treat as equal
  }
  if (move_get_row_start(a) != move_get_row_start(b) ||
      move_get_col_start(a) != move_get_col_start(b) ||
      move_get_dir(a) != move_get_dir(b) ||
      move_get_tiles_length(a) != move_get_tiles_length(b)) {
    return false;
  }
  const int n = move_get_tiles_length(a);
  for (int i = 0; i < n; i++) {
    if (move_get_tile(a, i) != move_get_tile(b, i)) {
      return false;
    }
  }
  return true;
}

// ── Position generation ──────────────────────────────────────────────────────

// A generated PEG benchmark position: the inferring player is on turn in the
// PEG bag range, right after an inferable opponent move.
typedef struct {
  Game *game;             // inferring player on turn, bag in PEG range
  Game *game_before_prev; // state before the opponent's prev move
  Move prev_move;         // opponent's prev move
  int prev_player;        // opponent index
  int prev_turn_bag;      // bag when the opponent moved (sim vs peg inference)
  int inferring_player;   // player on turn in `game`
} PegBenchPosition;

static void peg_bench_position_destroy(PegBenchPosition *p) {
  game_destroy(p->game);
  game_destroy(p->game_before_prev);
}

// Play static-eval games and capture states where the player on turn is in the
// PEG bag range right after an inferable opponent move, filling a per-class
// quota: sim-inf positions (opponent moved with a bag >= PEG_BENCH_SIM_INFER_BAG
// -- the transition into the PEG zone) and peg-inf positions (PEG->PEG, opponent
// moved with a bag <= 4 having played few tiles). Peg-inf positions are rare in
// static play, so many games are scanned. Returns the number captured.
// Generation-policy SIM move: sim the top static candidates (2-ply BAI, same
// shape as the inference's inner sims) and play the best by win%. Mirrors
// RUN_INNER_SIM in simmed_inference.c. Returns false (caller falls back to
// static) on an empty field or sim error.
static bool peg_bench_sim_move(Game *game, MoveList *move_list,
                               WinPct *win_pcts,
                               ThreadControl *thread_control, int iters,
                               uint64_t seed, Move *out) {
  move_list_reset(move_list);
  const MoveGenArgs gen_args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      .tiles_played_bv = NULL,
  };
  generate_moves(&gen_args);
  move_list_sort_moves(move_list);
  int arms = move_list_get_count(move_list);
  if (arms == 0) {
    return false;
  }
  if (arms == 1) {
    move_copy(out, move_list_get_move(move_list, 0));
    return true;
  }
  if (arms > 8) {
    arms = 8;
  }
  move_list->count = arms; // sim only the top arms (peg_inference.c pattern)
  thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED);
  SimArgs sa;
  sim_args_fill(/*num_plies=*/2, move_list, arms, /*known_opp_rack=*/NULL,
                win_pcts, /*inference_results=*/NULL, thread_control, game,
                /*sim_with_inference=*/false, /*use_heat_map=*/false,
                g_bench_num_threads, /*print_interval=*/0,
                /*max_display_plays=*/arms, /*max_display_plies=*/2, seed,
                (uint64_t)iters, /*min_play_iterations=*/1, /*scond=*/101.0,
                BAI_THRESHOLD_NONE, /*time_limit_seconds=*/9999,
                BAI_SAMPLING_RULE_ROUND_ROBIN, /*cutoff=*/0.0,
                /*inference_args=*/NULL, &sa);
  SimResults *sr = sim_results_create(0.0);
  ErrorStack *es = error_stack_create();
  simulate_without_ctx(&sa, sr, es);
  bool ok = error_stack_is_empty(es);
  if (ok) {
    int best = 0;
    double best_wp = -1.0;
    for (int i = 0; i < arms; i++) {
      const double wp = stat_get_mean(simmed_play_get_win_pct_stat(
          sim_results_get_simmed_play(sr, i)));
      if (wp > best_wp) {
        best_wp = wp;
        best = i;
      }
    }
    // Read the move off the simmed play itself (robust to any result
    // reordering inside the simmer).
    move_copy(out, simmed_play_get_move(sim_results_get_simmed_play(sr, best)));
  }
  error_stack_destroy(es);
  sim_results_destroy(sr);
  return ok;
}

// Generation-policy PEG move: a uniform peg_solve at the generation budget.
// Reuses peg_bench_best_move, temporarily overriding its budget global (test
// code; single-threaded generation loop).
static bool peg_bench_gen_peg_move(Game *game, MoveList *move_list,
                                   ThreadControl *thread_control, uint64_t seed,
                                   Move *out, ErrorStack *es) {
  const double saved_budget = g_bench_budget_s;
  g_bench_budget_s = g_bench_gen_pegbudget;
  double w = 0.0, s = 0.0;
  int st = -1;
  const bool ok = peg_bench_best_move(game, move_list, thread_control, NULL, 0,
                                      seed, out, &w, &s, &st, es);
  g_bench_budget_s = saved_budget;
  if (!ok) {
    error_stack_reset(es); // fall back to static, don't poison the caller
  }
  return ok;
}

static int peg_bench_generate(Game *proto, MoveList *move_list,
                              WinPct *win_pcts,
                              ThreadControl *thread_control,
                              PegBenchPosition *out, int max_sim, int max_peg,
                              uint64_t base_seed, uint64_t game_cap) {
  int n = 0;
  int n_sim = 0;
  int n_peg = 0;
  // Deterministic per-move seed and scratch error stack for the realistic
  // generation policy (see g_bench_realopp).
  uint64_t gen_turn_seed = base_seed + 777000000u;
  ErrorStack *gen_es = error_stack_create();
  for (uint64_t g = 0; (n_sim < max_sim || n_peg < max_peg) && g < game_cap;
       g++) {
    game_reset(proto);
    game_seed(proto, base_seed + g);
    draw_starting_racks(proto);
    Game *before_prev = game_duplicate(proto);
    Move prev_move;
    memset(&prev_move, 0, sizeof(prev_move));
    bool have_prev = false;
    int prev_player = -1;
    int prev_turn_bag = 0;
    while (!game_over(proto) && (n_sim < max_sim || n_peg < max_peg)) {
      const int on_turn = game_get_player_on_turn_index(proto);
      const int bag = bag_get_letters(game_get_bag(proto));
      const bool inferable =
          have_prev &&
          (move_get_type(&prev_move) == GAME_EVENT_TILE_PLACEMENT_MOVE ||
           move_get_type(&prev_move) == GAME_EVENT_EXCHANGE);
      const int score_gap =
          abs(equity_to_int(player_get_score(game_get_player(proto, 0))) -
              equity_to_int(player_get_score(game_get_player(proto, 1))));
      const bool contested =
          g_bench_contested <= 0 || score_gap <= g_bench_contested;
      if (inferable && contested && bag >= PEG_MIN_BAG &&
          bag <= PEG_MAX_BAG) {
        const bool sim_inf = prev_turn_bag >= PEG_BENCH_SIM_INFER_BAG;
        if ((sim_inf && n_sim < max_sim) || (!sim_inf && n_peg < max_peg)) {
          PegBenchPosition *p = &out[n++];
          p->game = game_duplicate(proto);
          p->game_before_prev = game_duplicate(before_prev);
          move_copy(&p->prev_move, &prev_move);
          p->prev_player = prev_player;
          p->prev_turn_bag = prev_turn_bag;
          p->inferring_player = on_turn;
          if (sim_inf) {
            n_sim++;
          } else {
            n_peg++;
          }
        }
      }
      Game *snap = game_duplicate(proto);
      Move played;
      bool picked = false;
      // Realistic policy inside the window the inference conditions on: SIM
      // moves at bag >= 5, uniform PEG moves at bag <= 4. Static equity above
      // the window and as the fallback on any solver failure.
      if (g_bench_realopp && bag >= 1 && bag <= g_bench_gen_authbag) {
        gen_turn_seed++;
        if (bag >= PEG_BENCH_SIM_INFER_BAG) {
          picked = peg_bench_sim_move(proto, move_list, win_pcts,
                                      thread_control, g_bench_gen_simiters,
                                      gen_turn_seed, &played);
        } else {
          picked = peg_bench_gen_peg_move(proto, move_list, thread_control,
                                          gen_turn_seed, &played, gen_es);
        }
      }
      if (!picked) {
        move_copy(&played, get_top_equity_move(proto, move_list));
      }
      play_move(&played, proto, NULL);
      game_copy(before_prev, snap);
      game_destroy(snap);
      prev_move = played;
      have_prev = true;
      prev_player = on_turn;
      prev_turn_bag = bag;
    }
    game_destroy(before_prev);
  }
  error_stack_destroy(gen_es);
  return n;
}

void test_peginf_benchmark(void) {
  char config_cmd[256];
  snprintf(config_cmd, sizeof(config_cmd),
           "set -lex CSW24 -s1 equity -s2 equity -r1 all -r2 all -numplays 15 "
           "-threads %d",
           get_num_cores());
  Config *config = config_create_or_die(config_cmd);
  load_and_exec_config_or_die(config, PEG_BENCH_4BAG_CGP ";");
  Game *game = config_get_game(config);
  ThreadControl *thread_control = config_get_thread_control(config);
  MoveList *move_list = move_list_create(64);
  ErrorStack *error_stack = error_stack_create();
  WinPct *win_pcts =
      win_pct_create(config_get_data_paths(config), DEFAULT_WIN_PCT,
                     error_stack);
  assert(error_stack_is_empty(error_stack));
  g_bench_num_threads = config_get_num_threads(config);
  g_bench_budget_s =
      peg_bench_env_double("PEGBENCH_BUDGET", PEG_BENCH_TURN_BUDGET_S);
  g_bench_samples =
      peg_bench_env_int("PEGBENCH_SAMPLES", PEG_BENCH_INFERENCE_SAMPLES);
  g_bench_psychic = peg_bench_env_bool("PEGBENCH_PSYCHIC", false);
  g_bench_greedy = peg_bench_env_bool("PEGBENCH_GREEDY", false);

  printf("  PEG A/B benchmark (4-in-bag CSW24, budget %.1fs/turn, %d threads):\n",
         g_bench_budget_s, g_bench_num_threads);

  // Run the A/B for each player as the inferring player. In this position p0
  // moves first (no opponent prior move -> inference is a no-op on its only PEG
  // turn), while p1 gets a PEG turn right after p0's move -> its peg-inference
  // path fires. Position generation (a later increment) yields positions where
  // the on-turn player always has the opponent's prior move to infer from.
  for (int inferring_player = 0; inferring_player < 2; inferring_player++) {
    printf("  === inferring player p%d ===\n", inferring_player);

    printf("  Arm A (uniform PEG):\n");
    Game *game_a = game_duplicate(game);
    const int spread_a =
        peg_bench_play_out(game_a, move_list, thread_control, win_pcts,
                           inferring_player, /*use_inference=*/false,
                           /*init=*/NULL, /*verbose=*/true, error_stack);

    printf("  Arm B (inference-weighted PEG):\n");
    Game *game_b = game_duplicate(game);
    const int spread_b =
        peg_bench_play_out(game_b, move_list, thread_control, win_pcts,
                           inferring_player, /*use_inference=*/true,
                           /*init=*/NULL, /*verbose=*/true, error_stack);

    printf("  RESULT p%d spread: A (uniform) %+d  vs  B (inference) %+d  "
           "(delta %+d)\n",
           inferring_player, spread_a, spread_b, spread_b - spread_a);

    assert(game_over(game_a));
    assert(game_over(game_b));
    assert(error_stack_is_empty(error_stack));
    game_destroy(game_a);
    game_destroy(game_b);
  }

  win_pct_destroy(win_pcts);
  move_list_destroy(move_list);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

// Generate PEG positions and aggregate the A/B (uniform vs inference) playout
// over them: for each position, play both arms to the end from the identical
// captured state (same bag order) and tally the inferring player's spread delta,
// split by inference type. The A/B signal -- whether inference improves play --
// emerges from this aggregate, not from any single position.
// Win points from the inferring player's final spread: win 1.0, tie 0.5, loss 0.
static double peg_bench_win_points(int spread) {
  if (spread > 0) {
    return 1.0;
  }
  return spread == 0 ? 0.5 : 0.0;
}

// Per-class (and overall) A/B tallies.
typedef struct {
  int n;
  long sum_delta;   // sum of (spread_b - spread_a)
  int b_better;     // delta > 0
  int a_better;     // delta < 0
  int ties;         // delta == 0
  double a_winpts;  // sum of win points for arm A
  double b_winpts;  // sum of win points for arm B
} PegBenchTally;

static void peg_bench_tally_add(PegBenchTally *t, int spread_a, int spread_b) {
  t->n++;
  const int delta = spread_b - spread_a;
  t->sum_delta += delta;
  if (delta > 0) {
    t->b_better++;
  } else if (delta < 0) {
    t->a_better++;
  } else {
    t->ties++;
  }
  t->a_winpts += peg_bench_win_points(spread_a);
  t->b_winpts += peg_bench_win_points(spread_b);
}

static void peg_bench_tally_print(const char *label, const PegBenchTally *t) {
  if (t->n == 0) {
    printf("  %s: no positions\n", label);
    return;
  }
  const int decisive = t->b_better + t->a_better;
  printf("  %s (n=%d): win%% A %.1f%% vs B %.1f%% (delta %+.1f pts); "
         "mean spread delta %+.1f; B better %d, A better %d, tie %d",
         label, t->n, 100.0 * t->a_winpts / t->n, 100.0 * t->b_winpts / t->n,
         t->b_winpts - t->a_winpts, (double)t->sum_delta / t->n, t->b_better,
         t->a_better, t->ties);
  if (decisive > 0) {
    printf(" (of %d decisive: B %.0f%%)", decisive,
           100.0 * t->b_better / decisive);
  }
  printf("\n");
}

void test_peginf_benchmark_generate(void) {
  char config_cmd[256];
  snprintf(config_cmd, sizeof(config_cmd),
           "set -lex CSW24 -s1 equity -s2 equity -r1 all -r2 all -numplays 15 "
           "-threads %d",
           get_num_cores());
  Config *config = config_create_or_die(config_cmd);
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  Game *game = config_get_game(config);
  ThreadControl *thread_control = config_get_thread_control(config);
  MoveList *move_list = move_list_create(64);
  ErrorStack *error_stack = error_stack_create();
  WinPct *win_pcts = win_pct_create(config_get_data_paths(config),
                                    DEFAULT_WIN_PCT, error_stack);
  assert(error_stack_is_empty(error_stack));
  g_bench_num_threads = config_get_num_threads(config);
  g_bench_budget_s =
      peg_bench_env_double("PEGBENCH_BUDGET", PEG_BENCH_TURN_BUDGET_S);
  g_bench_samples =
      peg_bench_env_int("PEGBENCH_SAMPLES", PEG_BENCH_INFERENCE_SAMPLES);
  g_bench_psychic = peg_bench_env_bool("PEGBENCH_PSYCHIC", false);
  g_bench_greedy = peg_bench_env_bool("PEGBENCH_GREEDY", false);

  // Quotas, seed, and game cap are env-overridable (PEGBENCH_SIM / _PEG / _SEED
  // / _GAMECAP) so a large run needs no rebuild.
  const int max_sim = peg_bench_env_int("PEGBENCH_SIM", PEG_BENCH_MAX_SIM);
  const int max_peg = peg_bench_env_int("PEGBENCH_PEG", PEG_BENCH_MAX_PEG);
  const uint64_t base_seed =
      (uint64_t)peg_bench_env_int("PEGBENCH_SEED", 1);
  const uint64_t game_cap =
      (uint64_t)peg_bench_env_int("PEGBENCH_GAMECAP", PEG_BENCH_GAME_CAP);

  PegBenchPosition *positions =
      malloc_or_die(sizeof(PegBenchPosition) * (size_t)(max_sim + max_peg));
  const int n = peg_bench_generate(game, move_list, win_pcts, thread_control,
                                   positions, max_sim, max_peg, base_seed,
                                   game_cap);
  printf("  PEG A/B aggregation: generated %d positions (target %d sim + %d "
         "peg), budget %.1fs/turn, %d threads, arm B = %s, move-sel = %s\n",
         n, max_sim, max_peg, g_bench_budget_s, g_bench_num_threads,
         g_bench_psychic ? "PSYCHIC (true opponent leave)" : "inference",
         g_bench_greedy ? "greedy stage-0" : "full halving stages");

  // Debug: skip playing positions before this index (generation is
  // deterministic, so this jumps straight to a specific position to reproduce).
  const int start = peg_bench_env_int("PEGBENCH_START", 0);

  PegBenchTally overall = {0};
  PegBenchTally sim = {0};
  PegBenchTally peg = {0};
  const double t_start = peg_bench_now_s();
  for (int i = 0; i < n; i++) {
    PegBenchPosition *p = &positions[i];
    if (i < start) {
      peg_bench_position_destroy(p);
      continue;
    }
    const bool sim_inf = p->prev_turn_bag >= PEG_BENCH_SIM_INFER_BAG;
    const PegBenchPrevCtx ctx = {
        .game_before_prev = p->game_before_prev,
        .prev_move = &p->prev_move,
        .prev_player = p->prev_player,
        .prev_turn_bag = p->prev_turn_bag,
    };

    Game *game_a = game_duplicate(p->game);
    const int spread_a = peg_bench_play_out(
        game_a, move_list, thread_control, win_pcts, p->inferring_player,
        /*use_inference=*/false, &ctx, /*verbose=*/false, error_stack);
    Game *game_b = game_duplicate(p->game);
    const int spread_b = peg_bench_play_out(
        game_b, move_list, thread_control, win_pcts, p->inferring_player,
        /*use_inference=*/true, &ctx, /*verbose=*/false, error_stack);

    peg_bench_tally_add(&overall, spread_a, spread_b);
    peg_bench_tally_add(sim_inf ? &sim : &peg, spread_a, spread_b);
    printf("    pos %2d  infer p%d  opp-bag=%d  %s-inf  A %+d  B %+d  "
           "delta %+d\n",
           i, p->inferring_player, p->prev_turn_bag, sim_inf ? "sim" : "peg",
           spread_a, spread_b, spread_b - spread_a);

    assert(game_over(game_a));
    assert(game_over(game_b));
    game_destroy(game_a);
    game_destroy(game_b);
    peg_bench_position_destroy(p);
  }

  printf("  --- A/B results (%.0fs) ---\n", peg_bench_now_s() - t_start);
  peg_bench_tally_print("OVERALL", &overall);
  peg_bench_tally_print("sim-inf", &sim);
  peg_bench_tally_print("peg-inf", &peg);
  assert(error_stack_is_empty(error_stack));

  free(positions);
  win_pct_destroy(win_pcts);
  move_list_destroy(move_list);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

// Aggregated top-N: collect the N distinct leaves with the largest SUMMED mass
// (duplicates aggregated) into out_racks/out_weights (descending). Returns the
// number filled (0 on empty support). O(n^2) rack compares; supports are a few
// hundred items.
static int peg_bench_top_leaves(AliasMethod *am, int ld_size, int topn,
                                Rack *out_racks, int64_t *out_weights) {
  const int n = alias_method_num_items(am);
  if (n <= 0 || topn <= 0) {
    return 0;
  }
  Rack item;
  rack_set_dist_size_and_reset(&item, ld_size);
  Rack probe;
  rack_set_dist_size_and_reset(&probe, ld_size);
  int n_out = 0;
  for (int i = 0; i < n; i++) {
    rack_reset(&item);
    alias_method_get_item(am, i, &item);
    bool seen = false;
    for (int j = 0; j < i && !seen; j++) {
      rack_reset(&probe);
      alias_method_get_item(am, j, &probe);
      seen = racks_are_equal(&probe, &item);
    }
    if (seen) {
      continue;
    }
    int64_t sum = 0;
    for (int j = i; j < n; j++) {
      rack_reset(&probe);
      const uint32_t w = alias_method_get_item(am, j, &probe);
      if (racks_are_equal(&probe, &item)) {
        sum += (int64_t)w;
      }
    }
    // Insert into the sorted top-N.
    if (n_out < topn || sum > out_weights[topn - 1]) {
      int pos = n_out < topn ? n_out : topn - 1;
      if (n_out < topn) {
        n_out++;
      }
      while (pos > 0 && out_weights[pos - 1] < sum) {
        rack_copy(&out_racks[pos], &out_racks[pos - 1]);
        out_weights[pos] = out_weights[pos - 1];
        pos--;
      }
      rack_copy(&out_racks[pos], &item);
      out_weights[pos] = sum;
    }
  }
  return n_out;
}

// The inference support may contain DUPLICATE entries for the same leave (one
// alias item per accepted evaluation). Aggregate by distinct leave and return
// the leave with the largest SUMMED mass in *out, its aggregated share in
// *out_share, and the number of distinct leaves. O(n^2) rack compares; supports
// are a few hundred items. Returns false on an empty support.
static bool peg_bench_top_leave(AliasMethod *am, int ld_size, Rack *out,
                                double *out_share, int *out_distinct) {
  const int n = alias_method_num_items(am);
  if (n <= 0) {
    return false;
  }
  Rack item;
  rack_set_dist_size_and_reset(&item, ld_size);
  Rack probe;
  rack_set_dist_size_and_reset(&probe, ld_size);
  int64_t total = 0;
  for (int i = 0; i < n; i++) {
    total += (int64_t)alias_method_item_count(am, i);
  }
  if (total <= 0) {
    return false;
  }
  int64_t best_sum = -1;
  int distinct = 0;
  for (int i = 0; i < n; i++) {
    rack_reset(&item);
    alias_method_get_item(am, i, &item);
    // Count each distinct leave once: skip if an identical rack appeared
    // earlier in the list.
    bool seen = false;
    for (int j = 0; j < i && !seen; j++) {
      rack_reset(&probe);
      alias_method_get_item(am, j, &probe);
      seen = racks_are_equal(&probe, &item);
    }
    if (seen) {
      continue;
    }
    distinct++;
    int64_t sum = 0;
    for (int j = i; j < n; j++) {
      rack_reset(&probe);
      const uint32_t w = alias_method_get_item(am, j, &probe);
      if (racks_are_equal(&probe, &item)) {
        sum += (int64_t)w;
      }
    }
    if (sum > best_sum) {
      best_sum = sum;
      rack_copy(out, &item);
    }
  }
  *out_share = (double)best_sum / (double)total;
  if (out_distinct != NULL) {
    *out_distinct = distinct;
  }
  return true;
}

// Record one calibration observation: how the inferred leave distribution
// relates to the KNOWN true leave. Emits a machine-parseable [calib] line:
//   k          true-leave size (information content proxy)
//   bag        bag size when the opponent moved (sim- vs peg-inference regime)
//   n_leaves   support size of the inferred distribution
//   conf       top-leave mass share (the runtime-computable confidence stat)
//   found      1 if the true leave is in the support at all
//   true_mass  prior mass assigned to the true leave
//   true_rank  1-based weight rank of the true leave (0 = not found)
static void peg_bench_calib_record(InferenceResults *results,
                                   const Rack *true_leave, int k, int prev_bag,
                                   const LetterDistribution *ld) {
  AliasMethod *am = inference_results_get_alias_method(results);
  const int n = alias_method_num_items(am);
  const int ld_size = ld_get_size(ld);
  Rack item;
  rack_set_dist_size_and_reset(&item, ld_size);
  Rack probe;
  rack_set_dist_size_and_reset(&probe, ld_size);
  int64_t total = 0;
  int64_t true_w = 0;
  for (int i = 0; i < n; i++) {
    rack_reset(&item);
    const uint32_t w = alias_method_get_item(am, i, &item);
    total += (int64_t)w;
    if (racks_are_equal(&item, true_leave)) {
      true_w += (int64_t)w; // duplicates: sum the leave's total mass
    }
  }
  // Aggregated top leave (duplicates summed) -- the leave a top-1 gate would
  // actually act on -- and the true leave's rank among DISTINCT leaves by
  // aggregated mass.
  Rack top_rack;
  rack_set_dist_size_and_reset(&top_rack, ld_size);
  double conf = 0.0;
  int n_distinct = 0;
  const bool has_top =
      peg_bench_top_leave(am, ld_size, &top_rack, &conf, &n_distinct);
  int rank = 0;
  if (true_w > 0) {
    rank = 1;
    for (int i = 0; i < n; i++) {
      rack_reset(&item);
      alias_method_get_item(am, i, &item);
      if (racks_are_equal(&item, true_leave)) {
        continue;
      }
      bool seen = false;
      for (int j = 0; j < i && !seen; j++) {
        rack_reset(&probe);
        alias_method_get_item(am, j, &probe);
        seen = racks_are_equal(&probe, &item);
      }
      if (seen) {
        continue;
      }
      int64_t sum = 0;
      for (int j = i; j < n; j++) {
        rack_reset(&probe);
        const uint32_t w = alias_method_get_item(am, j, &probe);
        if (racks_are_equal(&probe, &item)) {
          sum += (int64_t)w;
        }
      }
      if (sum > true_w) {
        rank++;
      }
    }
  }
  const bool top1_match = has_top && racks_are_equal(&top_rack, true_leave);
  StringBuilder *sb = string_builder_create();
  string_builder_add_rack(sb, true_leave, ld, false);
  printf("[calib] k=%d bag=%d type=%s n_items=%d n_distinct=%d conf=%.4f "
         "found=%d true_mass=%.5f true_rank=%d top1_match=%d leave=%s\n",
         k, prev_bag, prev_bag >= PEG_BENCH_SIM_INFER_BAG ? "sim" : "peg", n,
         n_distinct, conf, true_w > 0 ? 1 : 0,
         total > 0 ? (double)true_w / (double)total : 0.0, rank,
         top1_match ? 1 : 0, k == 0 ? "(empty)" : string_builder_peek(sb));
  fflush(stdout);
  string_builder_destroy(sb);
}

// Ceiling diagnostic (PEGBENCH_CEILING): GT-evaluate the top-K static candidates
// DIRECTLY against the true opponent leave -- the exact same evaluation GT uses
// to score the arms -- to find the GT-optimal move. Reports where the uniform
// (A) and psychic (B) solver picks land relative to that ceiling. If the ceiling
// (GT_best - GT_uniform) is large but psychic captures little of it, the solver
// is failing to convert perfect information it provably has -- the handicap is
// in peg_solve's application, not in the information.
static void peg_bench_ceiling_analysis(const Game *pos, const Rack *actual_leave,
                                       int opp_idx, int inferring_player,
                                       const Move *move_a, const Move *move_b,
                                       int gt_samples, uint64_t gt_seed,
                                       MoveList *move_list,
                                       ThreadControl *thread_control,
                                       double endgame_budget,
                                       ErrorStack *error_stack) {
  move_list_reset(move_list);
  const MoveGenArgs gen_args = {
      .game = (Game *)pos,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      .tiles_played_bv = NULL,
  };
  generate_moves(&gen_args);
  move_list_sort_moves(move_list);
  const int total = move_list_get_count(move_list);
  const int k = total < 10 ? total : 10;
  // Copy candidates out: peg_bench_ground_truth reuses move_list internally.
  Move cands[10];
  for (int i = 0; i < k; i++) {
    move_copy(&cands[i], move_list_get_move(move_list, i));
  }
  const Board *board = game_get_board(pos);
  const LetterDistribution *ld = game_get_ld(pos);
  StringBuilder *sb = string_builder_create();
  double best_gt = -1.0, a_gt = -1.0, b_gt = -1.0;
  int best_idx = 0;
  printf("      [CEILING] GT-win of top-%d static cands (opp=true leave, "
         "%d draws):\n",
         k, gt_samples);
  for (int i = 0; i < k; i++) {
    double sp, win;
    peg_bench_ground_truth(pos, &cands[i], actual_leave, opp_idx,
                           inferring_player, gt_samples, gt_seed, move_list,
                           thread_control, endgame_budget, error_stack, &sp,
                           &win);
    if (win > best_gt) {
      best_gt = win;
      best_idx = i;
    }
    const bool is_a = peg_bench_moves_equal(&cands[i], move_a);
    const bool is_b = peg_bench_moves_equal(&cands[i], move_b);
    if (is_a) {
      a_gt = win;
    }
    if (is_b) {
      b_gt = win;
    }
    string_builder_clear(sb);
    string_builder_add_move(sb, board, &cands[i], ld, true);
    printf("        %-18s gtWin=%.3f gtSpread=%+6.1f%s%s\n",
           string_builder_peek(sb), win, sp, is_a ? " [A-uniform]" : "",
           is_b ? " [B-psychic]" : "");
  }
  // The solver picks may sit below the top-K static field (a low-static,
  // high-win% play); GT them directly so the comparison is complete.
  double sp_tmp;
  if (a_gt < 0.0) {
    peg_bench_ground_truth(pos, move_a, actual_leave, opp_idx, inferring_player,
                           gt_samples, gt_seed, move_list, thread_control,
                           endgame_budget, error_stack, &sp_tmp, &a_gt);
  }
  if (b_gt < 0.0) {
    peg_bench_ground_truth(pos, move_b, actual_leave, opp_idx, inferring_player,
                           gt_samples, gt_seed, move_list, thread_control,
                           endgame_budget, error_stack, &sp_tmp, &b_gt);
  }
  double ceil_gt = best_gt;
  if (a_gt > ceil_gt) {
    ceil_gt = a_gt;
  }
  if (b_gt > ceil_gt) {
    ceil_gt = b_gt;
  }
  string_builder_clear(sb);
  string_builder_add_move(sb, board, &cands[best_idx], ld, true);
  printf("      [CEILING] GT-best(top-static)=%s %.3f | uniform=%.3f "
         "psychic=%.3f | ceiling=%+.3f  psychic-capture=%+.3f\n",
         string_builder_peek(sb), best_gt, a_gt, b_gt, ceil_gt - a_gt,
         b_gt - a_gt);
  string_builder_destroy(sb);
}

// Ground-truth benchmark: for each contested PEG position, compute the uniform
// move (arm A) and the inference/psychic move (arm B), then measure each move's
// GROUND-TRUTH expected result by Monte-Carlo over random draws that complete
// the opponent's ACTUAL leave. Compares the two moves' expected win% and spread
// -- the definitive "does the inference move actually win more often" test,
// with single-playout variance averaged out.
void test_peginf_benchmark_groundtruth(void) {
  char config_cmd[256];
  snprintf(config_cmd, sizeof(config_cmd),
           "set -lex CSW24 -s1 equity -s2 equity -r1 all -r2 all -numplays 15 "
           "-threads %d",
           get_num_cores());
  Config *config = config_create_or_die(config_cmd);
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  Game *game = config_get_game(config);
  ThreadControl *thread_control = config_get_thread_control(config);
  MoveList *move_list = move_list_create(64);
  ErrorStack *error_stack = error_stack_create();
  WinPct *win_pcts = win_pct_create(config_get_data_paths(config),
                                    DEFAULT_WIN_PCT, error_stack);
  assert(error_stack_is_empty(error_stack));
  g_bench_num_threads = config_get_num_threads(config);
  g_bench_budget_s =
      peg_bench_env_double("PEGBENCH_BUDGET", PEG_BENCH_TURN_BUDGET_S);
  g_bench_samples =
      peg_bench_env_int("PEGBENCH_SAMPLES", PEG_BENCH_INFERENCE_SAMPLES);
  g_bench_psychic = peg_bench_env_bool("PEGBENCH_PSYCHIC", true);
  g_bench_greedy = peg_bench_env_bool("PEGBENCH_GREEDY", false);
  g_bench_nest_depth =
      peg_bench_env_int("PEGBENCH_NESTDEPTH", PEG_NESTED_DEFAULT_DEPTH);
  g_bench_infer_topk = peg_bench_env_int("PEGBENCH_INFERTOPK", 0);
  g_bench_calib = peg_bench_env_int("PEGBENCH_CALIB", 0);
  g_bench_calib_kmin = peg_bench_env_int("PEGBENCH_CALIB_KMIN", 0);
  g_bench_calib_kmax = peg_bench_env_int("PEGBENCH_CALIB_KMAX", 7);
  g_bench_calib_simonly = peg_bench_env_bool("PEGBENCH_CALIB_SIMONLY", false);
  g_bench_inf_probe = peg_bench_env_int("PEGBENCH_INF_PROBE", 20);
  g_bench_inf_full = peg_bench_env_int("PEGBENCH_INF_FULL", 40);
  g_bench_peginf_solve_s = peg_bench_env_double("PEGBENCH_PEGINF_SOLVE", 0.1);
  g_bench_peginf_exh = peg_bench_env_int("PEGBENCH_PEGINF_EXH", 1);
  g_bench_realopp = peg_bench_env_bool("PEGBENCH_REALOPP", false);
  g_bench_gen_authbag = peg_bench_env_int("PEGBENCH_GEN_AUTHBAG", 12);
  g_bench_gen_simiters = peg_bench_env_int("PEGBENCH_GEN_SIMITERS", 100);
  g_bench_gen_pegbudget =
      peg_bench_env_double("PEGBENCH_GEN_PEGBUDGET", 5.0);
  g_bench_gate = peg_bench_env_bool("PEGBENCH_GATE", false);
  g_bench_gate_topn = peg_bench_env_int("PEGBENCH_GATE_TOPN", 4);
  g_bench_gate_kmin = peg_bench_env_int("PEGBENCH_GATE_KMIN", 3);
  g_bench_gate_kmax = peg_bench_env_int("PEGBENCH_GATE_KMAX", 6);
  g_bench_gate_obmin =
      peg_bench_env_int("PEGBENCH_GATE_OBMIN", PEG_BENCH_SIM_INFER_BAG);
  g_bench_gate_obmax = peg_bench_env_int("PEGBENCH_GATE_OBMAX", 99);
  g_bench_contested = peg_bench_env_int("PEGBENCH_CONTESTED", 25);
  g_bench_gt_samples = peg_bench_env_int("PEGBENCH_GT", 40);
  g_bench_gt_endgame = peg_bench_env_double("PEGBENCH_GTENDGAME", 2.0);

  const int max_sim = peg_bench_env_int("PEGBENCH_SIM", PEG_BENCH_MAX_SIM);
  const int max_peg = peg_bench_env_int("PEGBENCH_PEG", PEG_BENCH_MAX_PEG);
  const uint64_t base_seed = (uint64_t)peg_bench_env_int("PEGBENCH_SEED", 1);
  const uint64_t game_cap =
      (uint64_t)peg_bench_env_int("PEGBENCH_GAMECAP", PEG_BENCH_GAME_CAP);

  // Wall-clock time limit (PEGBENCH_TIMELIMIT, seconds; 0 = one batch). While
  // under the limit, keep generating fresh batches of contested positions and
  // evaluating them, so the run can proceed for hours.
  const double time_limit_s = peg_bench_env_double("PEGBENCH_TIMELIMIT", 0.0);

  PegBenchPosition *positions =
      malloc_or_die(sizeof(PegBenchPosition) * (size_t)(max_sim + max_peg));
  printf("  PEG ground-truth: contested |gap|<=%d, arm B = %s, %d MC draws/move, "
         "budget %.1fs, endgame %.1fs, %d threads, time-limit %.0fs\n",
         g_bench_contested, g_bench_psychic ? "PSYCHIC" : "inference",
         g_bench_gt_samples, g_bench_budget_s, g_bench_gt_endgame,
         g_bench_num_threads, time_limit_s);

  double sum_win_delta = 0.0;
  double sum_spread_delta = 0.0;
  int b_better = 0;
  int a_better = 0;
  int ties = 0;
  int total_n = 0;
  int same_move_n = 0;
  int decisive_n = 0;
  InferenceResults *inf_results = inference_results_create(NULL);
  StringBuilder *sb = string_builder_create();
  const LetterDistribution *ld = game_get_ld(game);
  const double t_start = peg_bench_now_s();
  uint64_t batch_seed = base_seed;
  int batch = 0;
  // Calibration-mode per-leave-size acceptance counters (index = true-leave
  // size, clamped to 7). See g_bench_calib.
  int calib_count[8] = {0};

  do {
    batch++;
    const int n = peg_bench_generate(game, move_list, win_pcts,
                                     thread_control, positions, max_sim,
                                     max_peg, batch_seed, game_cap);
    for (int i = 0; i < n; i++) {
      PegBenchPosition *p = &positions[i];
      const int opp_idx = p->prev_player;
      Rack actual_leave;
      peg_bench_true_leave(p->game_before_prev, &p->prev_move, p->prev_player,
                           &actual_leave);

      // Calibration mode: score the inference itself against the true leave,
      // stratified by leave size (skip cheaply once a bucket is full). No
      // solves, no playouts -- see g_bench_calib.
      if (g_bench_calib > 0) {
        int kb = (int)rack_get_total_letters(&actual_leave);
        if (kb > 7) {
          kb = 7;
        }
        const bool in_slice =
            kb >= g_bench_calib_kmin && kb <= g_bench_calib_kmax &&
            (!g_bench_calib_simonly ||
             p->prev_turn_bag >= PEG_BENCH_SIM_INFER_BAG);
        if (in_slice && calib_count[kb] < g_bench_calib) {
          const double t_inf = peg_bench_now_s();
          if (peg_bench_run_inference(
                  p->game_before_prev, &p->prev_move, p->prev_player,
                  p->prev_turn_bag, win_pcts, thread_control, inf_results,
                  g_bench_budget_s * PEG_BENCH_INFER_BUDGET_FRAC,
                  error_stack)) {
            printf("[calib-inf] elapsed=%.1fs ", peg_bench_now_s() - t_inf);
            peg_bench_calib_record(inf_results, &actual_leave, kb,
                                   p->prev_turn_bag, ld);
            calib_count[kb]++;
          }
        }
        peg_bench_position_destroy(p);
        continue;
      }

      const InferenceResults *prior = NULL;
      InferenceResults *psychic_res = NULL;
      InferenceResults *gated_res = NULL; // per-turn; destroyed with psychic_res
      AliasMethod *gate_alias = NULL;     // owned by us, not by gated_res
      int gate_k_est = -1;                // set when the gate fires
      // In gate mode the gate condition is fully observable BEFORE inference
      // (bag size + play length), so skip the expensive inference entirely on
      // gated-off positions -- exactly as a realtime engine would.
      bool gate_precheck = true;
      if (g_bench_gate && !g_bench_psychic) {
        const int pre_played = move_get_tiles_played(&p->prev_move);
        const Rack *pre_rack = player_get_rack(
            game_get_player(p->game_before_prev, p->prev_player));
        const int pre_k = (int)rack_get_total_letters(pre_rack) - pre_played;
        gate_precheck = p->prev_turn_bag >= g_bench_gate_obmin &&
                        p->prev_turn_bag <= g_bench_gate_obmax &&
                        pre_k >= g_bench_gate_kmin && pre_k <= g_bench_gate_kmax;
        if (getenv("PEGBENCH_PREDBG") != NULL) {
          printf("    [pre] bag=%d played=%d rack_before=%d pre_k=%d -> %d\n",
                 p->prev_turn_bag, pre_played,
                 (int)rack_get_total_letters(pre_rack), pre_k,
                 gate_precheck ? 1 : 0);
          fflush(stdout);
        }
      }
      if (g_bench_psychic) {
        psychic_res = peg_bench_psychic_prior(p->game_before_prev,
                                              &p->prev_move, p->prev_player);
        prior = psychic_res;
      } else if (gate_precheck &&
                 peg_bench_run_inference(
                     p->game_before_prev, &p->prev_move, p->prev_player,
                     p->prev_turn_bag, win_pcts, thread_control, inf_results,
                     g_bench_budget_s * PEG_BENCH_INFER_BUDGET_FRAC,
                     error_stack)) {
        prior = inf_results;
        // Play-length gate (PEGBENCH_GATE): arm B trusts the inference ONLY on
        // a SIM-inference play whose estimated kept leave k_est = rack_before -
        // tiles_played (both runtime-observable) lies in the configured slice
        // [kmin, kmax] (default 3..6: excludes bingos/k=0 and the big plays
        // where calibration shows the inference is least accurate). The prior
        // handed to peg_solve is the aggregated TOP-N leaves (duplicates
        // summed), which fits peg_solve's exact small-support enumeration.
        // Everything else falls back to uniform (prior = NULL => B == A).
        if (g_bench_gate) {
          prior = NULL;
          const int played = move_get_tiles_played(&p->prev_move);
          const Rack *before_rack =
              player_get_rack(game_get_player(p->game_before_prev,
                                              p->prev_player));
          const int k_est = (int)rack_get_total_letters(before_rack) - played;
          AliasMethod *am = inference_results_get_alias_method(inf_results);
          enum { GATE_MAX_TOPN = 8 };
          const int topn =
              g_bench_gate_topn > GATE_MAX_TOPN ? GATE_MAX_TOPN
                                                : g_bench_gate_topn;
          Rack top_racks[GATE_MAX_TOPN];
          int64_t top_ws[GATE_MAX_TOPN];
          for (int t = 0; t < GATE_MAX_TOPN; t++) {
            rack_set_dist_size_and_reset(&top_racks[t], ld_get_size(ld));
          }
          int n_top = 0;
          if (p->prev_turn_bag >= g_bench_gate_obmin &&
              p->prev_turn_bag <= g_bench_gate_obmax &&
              k_est >= g_bench_gate_kmin && k_est <= g_bench_gate_kmax) {
            n_top = peg_bench_top_leaves(am, ld_get_size(ld), topn, top_racks,
                                         top_ws);
          }
          if (n_top > 0) {
            AliasMethod *ga = alias_method_create();
            bool true_in_topn = false;
            for (int t = 0; t < n_top; t++) {
              alias_method_add_rack(ga, &top_racks[t], (int)top_ws[t]);
              if (racks_are_equal(&top_racks[t], &actual_leave)) {
                true_in_topn = true;
              }
            }
            if (alias_method_generate_tables(ga)) {
              // inference_results_destroy does NOT free a caller-supplied
              // alias; gate_alias is destroyed alongside gated_res below.
              gated_res = inference_results_create(ga);
              gate_alias = ga;
              prior = gated_res;
              gate_k_est = k_est;
              string_builder_clear(sb);
              string_builder_add_rack(sb, &top_racks[0], ld, false);
              printf("    [gate ON] k_est=%d topn=%d top1=%s in_topn=%d\n",
                     k_est, n_top, string_builder_peek(sb),
                     true_in_topn ? 1 : 0);
              fflush(stdout);
            } else {
              alias_method_destroy(ga);
            }
          }
        }
      }

      // Gate mode: a gated-off position contributes nothing (arm B == arm A by
      // construction), so skip BOTH solves -- the dominant throughput win for
      // the overnight run (gated-off positions previously burned a full arm-A
      // solve before being discarded).
      if (g_bench_gate && !g_bench_psychic && prior == NULL) {
        peg_bench_position_destroy(p);
        continue;
      }

      // Uniform move (arm A) and inference/psychic move (arm B).
      Game *g_solve = game_duplicate(p->game);
      Move move_a;
      Move move_b;
      double peg_a_win = -1.0, peg_a_spread = 0.0;
      double peg_b_win = -1.0, peg_b_spread = 0.0;
      int peg_a_stage = -1, peg_b_stage = -1;
      const bool ok_a = peg_bench_best_move(
          g_solve, move_list, thread_control, NULL, 0,
          batch_seed + (uint64_t)i, &move_a, &peg_a_win, &peg_a_spread,
          &peg_a_stage, error_stack);
      const bool ok_b =
          prior != NULL &&
          peg_bench_best_move(g_solve, move_list, thread_control, prior,
                              g_bench_samples, batch_seed + (uint64_t)i,
                              &move_b, &peg_b_win, &peg_b_spread, &peg_b_stage,
                              error_stack);
      game_destroy(g_solve);
      if (!ok_a || !ok_b) {
        if (psychic_res != NULL) {
          inference_results_destroy(psychic_res);
        }
        inference_results_destroy(gated_res);
        alias_method_destroy(gate_alias);
        peg_bench_position_destroy(p);
        continue;
      }

      // Per-position stage-reached probe (every position, not just decisive):
      // if this stays 0 the solve never got past the greedy seed and the deep
      // staged evaluation never ran -- the whole point of the real config.
      if (getenv("PEGBENCH_STAGEPROBE") != NULL) {
        printf("    [stageprobe] pos %d: A stage=%d B stage=%d\n", i,
               peg_a_stage, peg_b_stage);
        fflush(stdout);
      }

      // If the inference/psychic move is the same as the uniform move, the
      // ground truth is identical -- skip the (expensive, noise-only) MC.
      total_n++;
      if (peg_bench_moves_equal(&move_a, &move_b)) {
        same_move_n++;
        if (psychic_res != NULL) {
          inference_results_destroy(psychic_res);
        }
        inference_results_destroy(gated_res);
        alias_method_destroy(gate_alias);
        peg_bench_position_destroy(p);
        continue;
      }

      // Ground-truth expected value of each move (shared per-sample seeds =>
      // paired opponent completions).
      const uint64_t gt_seed = batch_seed + 1000u * (uint64_t)(i + 1);
      double gt_a_spread;
      double gt_a_win;
      double gt_b_spread;
      double gt_b_win;
      peg_bench_ground_truth(p->game, &move_a, &actual_leave, opp_idx,
                             p->inferring_player, g_bench_gt_samples, gt_seed,
                             move_list, thread_control, g_bench_gt_endgame,
                             error_stack, &gt_a_spread, &gt_a_win);
      peg_bench_ground_truth(p->game, &move_b, &actual_leave, opp_idx,
                             p->inferring_player, g_bench_gt_samples, gt_seed,
                             move_list, thread_control, g_bench_gt_endgame,
                             error_stack, &gt_b_spread, &gt_b_win);

      if (getenv("PEGBENCH_CEILING") != NULL) {
        peg_bench_ceiling_analysis(p->game, &actual_leave, opp_idx,
                                   p->inferring_player, &move_a, &move_b,
                                   g_bench_gt_samples, gt_seed, move_list,
                                   thread_control, g_bench_gt_endgame,
                                   error_stack);
      }

      const double win_delta = gt_b_win - gt_a_win;
      const double spread_delta = gt_b_spread - gt_a_spread;
      // Per-position slice record for gated runs: play-length slice analysis
      // reads these lines (only differing-move positions reach here; gated-on
      // same-move positions contribute delta 0 by construction).
      if (gate_k_est >= 0) {
        printf("    [gateGT] k_est=%d win_delta=%+.3f spread_delta=%+.1f\n",
               gate_k_est, win_delta, spread_delta);
        fflush(stdout);
      }
      sum_win_delta += win_delta;
      sum_spread_delta += spread_delta;
      if (win_delta > 1e-9) {
        b_better++;
      } else if (win_delta < -1e-9) {
        a_better++;
      } else {
        ties++;
      }

      // A position is "decisive" when the inference move differs from uniform
      // (paired seeds => identical results iff same move). Log full detail.
      const bool win_decisive = fabs(win_delta) > 1e-9;
      const bool decisive = win_decisive || fabs(spread_delta) > 0.5;
      if (decisive) {
        decisive_n++;
        const Rack *mover_rack =
            player_get_rack(game_get_player(p->game, p->inferring_player));
        printf("  === DECISIVE #%d (batch %d, opp-bag=%d, %s-inf)%s ===\n",
               decisive_n, batch, p->prev_turn_bag,
               p->prev_turn_bag >= PEG_BENCH_SIM_INFER_BAG ? "sim" : "peg",
               win_decisive ? "  [WIN-DELTA]" : "  [spread only]");
        string_builder_clear(sb);
        string_builder_add_rack(sb, mover_rack, ld, false);
        printf("      mover(p%d) rack: %s | ", p->inferring_player,
               string_builder_peek(sb));
        string_builder_clear(sb);
        string_builder_add_rack(sb, &actual_leave, ld, false);
        printf("opp(p%d) true leave: %s\n", opp_idx, string_builder_peek(sb));
        // Format moves against the captured position's board (not the shared
        // generation proto) so play-through tiles resolve correctly.
        const Board *pos_board = game_get_board(p->game);
        string_builder_clear(sb);
        string_builder_add_move(sb, pos_board, &move_a, ld, true);
        printf("      A uniform:  %-18s -> gtWin %.2f (%.1f/%d)  gtSpread %+.1f"
               "   [peg win %.2f sprd %+.1f]\n",
               string_builder_peek(sb), gt_a_win,
               gt_a_win * g_bench_gt_samples, g_bench_gt_samples, gt_a_spread,
               peg_a_win, peg_a_spread);
        string_builder_clear(sb);
        string_builder_add_move(sb, pos_board, &move_b, ld, true);
        printf("      B %-7s:  %-18s -> gtWin %.2f (%.1f/%d)  gtSpread %+.1f"
               "   [peg win %.2f sprd %+.1f]\n",
               g_bench_psychic ? "psychic" : "infer", string_builder_peek(sb),
               gt_b_win, gt_b_win * g_bench_gt_samples, g_bench_gt_samples,
               gt_b_spread, peg_b_win, peg_b_spread);
        printf("      peg stage reached: A=%d B=%d (0 = greedy seed only)\n",
               peg_a_stage, peg_b_stage);
        printf("      => win delta %+.2f, spread delta %+.1f  [%s]\n", win_delta,
               spread_delta,
               win_delta > 1e-9 ? "B (inference) better"
                                : (win_delta < -1e-9 ? "A (uniform) better"
                                                     : "win tie, spread only"));
        fflush(stdout);
      }

      if (psychic_res != NULL) {
        inference_results_destroy(psychic_res);
      }
      inference_results_destroy(gated_res);
      alias_method_destroy(gate_alias);
      peg_bench_position_destroy(p);
    }

    const double elapsed = peg_bench_now_s() - t_start;
    const int diff_n = total_n - same_move_n;
    printf("  [batch %d | %.0fs | total=%d same-move=%d differing=%d] "
           "winΔ over differing: B%d A%d tie%d; mean winΔ(all)=%+.4f\n",
           batch, elapsed, total_n, same_move_n, diff_n, b_better, a_better,
           ties, total_n ? sum_win_delta / total_n : 0.0);
    fflush(stdout);
    batch_seed += 1000000u;
    if (g_bench_calib > 0) {
      bool all_full = true;
      printf("  [calib coverage after batch %d]", batch);
      // k ranges 0..6: a placement plays >= 1 tile and an exchange keeps <= 6,
      // so a 7-tile leave is impossible (passes are not inferable) -- do not
      // wait on the k7 bucket, nor on buckets outside the calib slice.
      const int cov_min = g_bench_calib_kmin < 0 ? 0 : g_bench_calib_kmin;
      const int cov_max = g_bench_calib_kmax > 6 ? 6 : g_bench_calib_kmax;
      for (int kb = cov_min; kb <= cov_max; kb++) {
        printf(" k%d=%d/%d", kb, calib_count[kb], g_bench_calib);
        if (calib_count[kb] < g_bench_calib) {
          all_full = false;
        }
      }
      printf("\n");
      fflush(stdout);
      if (all_full) {
        break; // every possible leave-size bucket has its quota
      }
    }
  } while (time_limit_s > 0.0 && (peg_bench_now_s() - t_start) < time_limit_s);

  const int diff_n = total_n - same_move_n;
  if (total_n > 0) {
    printf("  GROUND-TRUTH FINAL (%d positions, %d same-move skipped, %d "
           "differing over %d batches):\n"
           "    mean win%% delta over ALL = %+.4f; over DIFFERING = %+.4f; "
           "mean spread delta (differing) = %+.2f\n"
           "    differing moves: B (psychic) better %d, A (uniform) better %d, "
           "tie %d\n",
           total_n, same_move_n, diff_n, batch, sum_win_delta / total_n,
           diff_n ? sum_win_delta / diff_n : 0.0,
           diff_n ? sum_spread_delta / diff_n : 0.0, b_better, a_better, ties);
  }
  assert(error_stack_is_empty(error_stack));

  string_builder_destroy(sb);
  inference_results_destroy(inf_results);
  free(positions);
  win_pct_destroy(win_pcts);
  move_list_destroy(move_list);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

// Diagnostic: run the d25 endgame on a single CGP (env PEGBENCH_CGP) repeatedly
// at a controllable thread count (PEGBENCH_THREADS) to isolate whether the
// zobrist_add_move overflow is a parallel-endgame race (crashes only with >1
// thread) or a deterministic position/depth bug (crashes single-threaded too).
void test_peginf_endgame_repro(void) {
  const char *cgp = getenv("PEGBENCH_CGP");
  if (cgp == NULL) {
    printf("  set PEGBENCH_CGP to a board+racks CGP\n");
    return;
  }
  const int threads = peg_bench_env_int("PEGBENCH_THREADS", get_num_cores());
  const int iters = peg_bench_env_int("PEGBENCH_ITERS", 30);
  char cmd[256];
  snprintf(cmd, sizeof(cmd),
           "set -lex CSW24 -s1 equity -s2 equity -r1 all -r2 all -numplays 15 "
           "-threads %d",
           threads);
  Config *config = config_create_or_die(cmd);
  char cgp_cmd[512];
  snprintf(cgp_cmd, sizeof(cgp_cmd), "cgp %s", cgp);
  load_and_exec_config_or_die(config, cgp_cmd);
  Game *game = config_get_game(config);
  ThreadControl *thread_control = config_get_thread_control(config);
  ErrorStack *error_stack = error_stack_create();
  g_bench_num_threads = threads;
  g_bench_budget_s =
      peg_bench_env_double("PEGBENCH_BUDGET", PEG_BENCH_TURN_BUDGET_S);

  printf("  endgame repro: %d threads, %d iters, d%d\n", threads, iters,
         PEG_BENCH_ENDGAME_PLIES);
  for (int i = 0; i < iters; i++) {
    Game *g = game_duplicate(game);
    Move mv;
    const bool played = peg_bench_play_endgame(
        g, thread_control, g_bench_budget_s, g_bench_num_threads, &mv,
        error_stack);
    printf("    iter %2d: played=%d err=%d\n", i, played,
           !error_stack_is_empty(error_stack));
    fflush(stdout);
    error_stack_reset(error_stack);
    game_destroy(g);
  }
  printf("  DONE (no crash over %d iters)\n", iters);

  error_stack_destroy(error_stack);
  config_destroy(config);
}
