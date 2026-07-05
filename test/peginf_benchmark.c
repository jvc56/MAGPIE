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
#include "../src/ent/win_pct.h"
#include "../src/impl/config.h"
#include "../src/impl/endgame.h"
#include "../src/impl/cgp.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/peg.h"
#include "../src/impl/peg_inference.h"
#include "../src/impl/simmed_inference.h"
#include "../src/compat/memory_info.h"
#include "../src/util/io_util.h"
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

// Read a positive integer environment override, or fall back to def.
static int peg_bench_env_int(const char *name, int def) {
  const char *v = getenv(name);
  if (v == NULL) {
    return def;
  }
  const int parsed = atoi(v);
  return parsed > 0 ? parsed : def;
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
  if (prev_turn_bag >= PEG_BENCH_SIM_INFER_BAG) {
    const SimmedInferenceArgs si_args = {
        .base = &setup.args,
        .observed_move = prev_move,
        .win_pcts = win_pcts,
        .num_candidate_plays = PEG_BENCH_INFER_CANDIDATES,
        .num_inner_sim_plies = 2,
        .probe_iterations = 20,
        .full_iterations = 40,
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
        .exhaustive_max_leave = 1,
        .peg_time_budget_s = 0.1,
        .time_budget_s = budget_s,
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

// Solve and play the endgame (bag empty), capped to budget_s. Copies the played
// move to *out_move. Returns true if a move was played.
static bool peg_bench_play_endgame(Game *game, ThreadControl *thread_control,
                                   double budget_s, Move *out_move,
                                   ErrorStack *error_stack) {
  EndgameArgs endgame_args = {0};
  endgame_args.thread_control = thread_control;
  endgame_args.game = game;
  endgame_args.plies = PEG_BENCH_ENDGAME_PLIES;
  endgame_args.tt_fraction_of_mem = 0.05;
  endgame_args.initial_small_move_arena_size =
      DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE;
  endgame_args.num_threads = g_bench_num_threads;
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
    played = peg_bench_play_endgame(game, thread_control, budget_s, out_move,
                                    error_stack);
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
      peg_args.greedy_seed_only = true;
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
    double infer_elapsed = 0.0;
    const bool prev_inferable =
        have_prev && (move_get_type(&prev_move) == GAME_EVENT_TILE_PLACEMENT_MOVE ||
                      move_get_type(&prev_move) == GAME_EVENT_EXCHANGE);
    if (use_inference && on_turn == inferring_player && prev_inferable &&
        turn_bag >= PEG_MIN_BAG && turn_bag <= PEG_MAX_BAG) {
      const double t0 = peg_bench_now_s();
      const double infer_budget = g_bench_budget_s * PEG_BENCH_INFER_BUDGET_FRAC;
      if (peg_bench_run_inference(game_before_prev, &prev_move, prev_player,
                                  prev_turn_bag, win_pcts, thread_control,
                                  inf_results, infer_budget, error_stack)) {
        prior = inf_results;
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
          prior != NULL ? "  [inf]" : "",
          turn_total > g_bench_budget_s * 1.5 ? "  *** OVER" : "");
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
static int peg_bench_generate(Game *proto, MoveList *move_list,
                              PegBenchPosition *out, int max_sim, int max_peg,
                              uint64_t base_seed, uint64_t game_cap) {
  int n = 0;
  int n_sim = 0;
  int n_peg = 0;
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
      if (inferable && bag >= PEG_MIN_BAG && bag <= PEG_MAX_BAG) {
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
      move_copy(&played, get_top_equity_move(proto, move_list));
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
  const int n = peg_bench_generate(game, move_list, positions, max_sim, max_peg,
                                   base_seed, game_cap);
  printf("  PEG A/B aggregation: generated %d positions (target %d sim + %d "
         "peg), budget %.1fs/turn, %d threads\n",
         n, max_sim, max_peg, g_bench_budget_s, g_bench_num_threads);

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
        g, thread_control, g_bench_budget_s, &mv, error_stack);
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
