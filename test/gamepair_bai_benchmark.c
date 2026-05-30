// Game-pair autoplay benchmark: static BAI vs nested-sim BAI.
// Usage: ./bin/magpie_test gamepairbai
//
// Plays 10 game pairs from an empty board. Each pair uses the same seed
// but swaps which player uses the static strategy vs the nested strategy.
// Every turn: generate 15 candidates → BAI with fixed time limit → play best.
// BAI uses NO threshold (BAI_THRESHOLD_NONE), only the time limit.

#include "../src/compat/ctime.h"
#include "../src/def/bai_defs.h"
#include "../src/def/config_defs.h"
#include "../src/def/game_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/sim_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/bai_result.h"
#include "../src/ent/board.h"
#include "../src/ent/endgame_results.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/sim_args.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/stats.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/win_pct.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/endgame.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/simmer.h"
#include "../src/str/game_string.h"
#include "../src/str/move_string.h"
#include "../src/str/rack_string.h"
#include "../src/str/sim_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_constants.h"
#include "test_util.h"
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#define NUM_GAME_PAIRS 1
#define NUM_PLAYS 50
#define OUTER_MIN_PLAY_ITERATIONS 10
#define OUTER_UTILITY_W_WINPCT 1.0
#define OUTER_UTILITY_W_SPREAD 0.1
#define OUTER_UTILITY_SPREAD_SCALE 100.0
#define STATIC_PLIES 4
#define NESTED_PLIES 4
#define NUM_THREADS 10
#define TURN_TIME_LIMIT_S 30
// After this many tiles are placed on the board, skip the hybrid probe and
// any nested sims — just play 2s flat sims to drive the game to completion.
#define LATE_GAME_TILES_THRESHOLD 70
#define LATE_GAME_FLAT_BUDGET_S 2.0
#define ENDGAME_PLIES 25
#define ENDGAME_TIME_LIMIT_S 10.0
#define LATE_GAME_TILE_THRESHOLD 21
#define LATE_GAME_PLIES 99

// Two strategies to compare. nested_candidates / nested_rollouts on the
// NESTED strategy are overridable via GAMEPAIRBAI_K / GAMEPAIRBAI_N env vars
// so the cost ratio (≈ K*N*nested_plies relative to a static-eval sample)
// can be tuned without recompiling.
static FidelityLevel STRATEGY_STATIC = {
    .sample_limit = UINT64_MAX,
    .sample_minimum = 1,
    .time_limit_seconds = TURN_TIME_LIMIT_S,
    .ply_strategy = PLY_STRATEGY_STATIC,
    .inner_w_winpct = 1.0,
    .inner_w_spread = 0.0,
    .inner_spread_scale = 100.0,
};

static FidelityLevel STRATEGY_NESTED = {
    .sample_limit = UINT64_MAX,
    .sample_minimum = 1,
    .time_limit_seconds = TURN_TIME_LIMIT_S,
    .ply_strategy = PLY_STRATEGY_NESTED_SIM,
    .nested_candidates = 40,
    .nested_rollouts = 3, // per-arm floor; TOP_TWO concentrates beyond this
    .nested_plies = 2,
    .nested_max_samples = 250, // ~15ms inner cap at observed ~55us/rollout
    .nested_stop_z = 2.326,    // one-sided 99% Welch t early stop
    .inner_w_winpct = 1.0,
    .inner_w_spread = 0.5,
    .inner_spread_scale = 100.0,
};

// HYBRID strategy: spend HYBRID_PROBE_SECS on a nested probe; if the mean
// inner-call loss is above HYBRID_THRESHOLD (i.e., position is "spicy" in
// the closed-board sense), continue with nested for the remaining budget;
// else switch to flat. Probe rollouts are discarded by phase 2 (simulate()
// resets sim_results); this is acceptable for a first-pass test.
static FidelityLevel STRATEGY_HYBRID = {
    .sample_limit = UINT64_MAX,
    .sample_minimum = 1,
    .time_limit_seconds = TURN_TIME_LIMIT_S, // total budget
    .ply_strategy = PLY_STRATEGY_NESTED_SIM, // sentinel; not actually used
    .nested_candidates = 40,
    .nested_rollouts = 3,
    .nested_plies = 2,
    .nested_max_samples = 250,
    .nested_stop_z = 2.326,
    .inner_w_winpct = 1.0,
    .inner_w_spread = 0.5,
    .inner_spread_scale = 100.0,
};
static double HYBRID_PROBE_SECS = 1.0;
static double HYBRID_THRESHOLD = 0.10;

// CSV output handle. Opened from GAMEPAIRBAI_CSV env var; NULL = no CSV.
static FILE *g_csv_file = NULL;

// Disagreement CSV output handle. Opened from GAMEPAIRBAI_DISAGREE_CSV env
// var; NULL = no disagreement logging. Each row records the first
// position-aligned turn within a pair where STATIC's and NESTED's picks
// diverge, plus cross-eval data from re-running both sims at that position.
static FILE *g_disagree_file = NULL;

// Max turns we'll track per game for divergence analysis (well above the
// typical ~25 turns/game).
#define MAX_TRACKED_TURNS 64

// Move comparison: ignores score/equity (which depend on board context) and
// just checks placement. Iterates `tiles_length` (the full word span,
// including through-tiles) since the tiles[] array is indexed by absolute
// position in the word; through-tile positions are marked specially but must
// still match between equivalent moves.
static bool moves_equivalent(const Move *a, const Move *b) {
  if (a->move_type != b->move_type) {
    return false;
  }
  if (a->move_type == GAME_EVENT_PASS) {
    return true;
  }
  if (a->row_start != b->row_start || a->col_start != b->col_start) {
    return false;
  }
  if (a->dir != b->dir) {
    return false;
  }
  if (a->tiles_played != b->tiles_played) {
    return false;
  }
  if (a->tiles_length != b->tiles_length) {
    return false;
  }
  for (int i = 0; i < a->tiles_length; i++) {
    if (a->tiles[i] != b->tiles[i]) {
      return false;
    }
  }
  return true;
}

// Snapshot of a sim's per-candidate moves and win-pct means.
typedef struct SimSnapshot {
  Move moves[NUM_PLAYS];
  double wpcts[NUM_PLAYS];
  int count;
} SimSnapshot;

static void snapshot_sim_results(const SimResults *sim_results, int num_plays,
                                 SimSnapshot *snap) {
  snap->count = (num_plays < NUM_PLAYS) ? num_plays : NUM_PLAYS;
  for (int i = 0; i < snap->count; i++) {
    const SimmedPlay *sp = sim_results_get_simmed_play(sim_results, i);
    snap->moves[i] = *simmed_play_get_move(sp);
    snap->wpcts[i] = stat_get_mean(simmed_play_get_win_pct_stat(sp));
  }
}

// Find target in a snapshot and report its win_pct mean and 1-indexed rank
// (ties broken by lower index = lower rank). Returns false if not found.
static bool find_move_in_snapshot(const SimSnapshot *snap, const Move *target,
                                  double *out_wpct, int *out_rank) {
  int target_idx = -1;
  for (int i = 0; i < snap->count; i++) {
    if (moves_equivalent(&snap->moves[i], target)) {
      target_idx = i;
      break;
    }
  }
  if (target_idx < 0) {
    *out_wpct = -1.0;
    *out_rank = -1;
    return false;
  }
  *out_wpct = snap->wpcts[target_idx];
  int rank = 1;
  for (int i = 0; i < snap->count; i++) {
    if (i != target_idx && snap->wpcts[i] > snap->wpcts[target_idx]) {
      rank++;
    }
  }
  *out_rank = rank;
  return true;
}

typedef struct {
  int p0_wins;
  int p1_wins;
  int ties;
  int total_turns;
  double total_elapsed_s;
} PairResults;

// Timer thread: sleeps for the specified duration, then fires USER_INTERRUPT.
typedef struct {
  ThreadControl *tc;
  double seconds;
  volatile bool done;
} TimerArgs;

static void *timer_thread_func(void *arg) {
  TimerArgs *ta = (TimerArgs *)arg;
  double remaining = ta->seconds;
  while (remaining > 0 && !ta->done) {
    double sleep_time = remaining > 0.05 ? 0.05 : remaining;
    struct timespec ts;
    ts.tv_sec = (time_t)sleep_time;
    ts.tv_nsec = (long)((sleep_time - (double)ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
    remaining -= sleep_time;
  }
  if (!ta->done) {
    thread_control_set_status(ta->tc, THREAD_CONTROL_STATUS_USER_INTERRUPT);
  }
  return NULL;
}

// Count tiles unseen by the player on turn (bag + opponent's rack).
static int tiles_unseen(const Game *game) {
  return bag_get_letters(game_get_bag(game)) +
         rack_get_total_letters(player_get_rack(
             game_get_player(game, 1 - game_get_player_on_turn_index(game))));
}

// Play one move using the endgame solver with a time limit.
static const Move *play_endgame_turn(Game *game, MoveList *move_list,
                                     EndgameCtx **ctx_ptr,
                                     EndgameResults *endgame_results,
                                     ThreadControl *tc,
                                     ErrorStack *error_stack) {
  EndgameArgs args = {
      .game = game,
      .thread_control = tc,
      .plies = ENDGAME_PLIES,
      .tt_fraction_of_mem = 0.05,
      .initial_small_move_arena_size = DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
      .num_threads = NUM_THREADS,
      .num_top_moves = 1,
      .use_heuristics = true,
      .forced_pass_bypass = true,
  };

  thread_control_set_status(tc, THREAD_CONTROL_STATUS_STARTED);
  endgame_results_reset(endgame_results);

  TimerArgs ta = {.tc = tc, .seconds = ENDGAME_TIME_LIMIT_S, .done = false};
  pthread_t timer_tid;
  pthread_create(&timer_tid, NULL, timer_thread_func, &ta);

  endgame_solve(ctx_ptr, &args, endgame_results, error_stack);

  ta.done = true;
  pthread_join(timer_tid, NULL);

  const PVLine *pv =
      endgame_results_get_pvline(endgame_results, ENDGAME_RESULT_BEST);
  Move *spare = move_list_get_spare_move(move_list);
  if (pv->num_moves > 0) {
    small_move_to_move(spare, &pv->moves[0], game_get_board(game));
  } else {
    move_set_as_pass(spare);
  }
  play_move(spare, game, NULL);
  return spare;
}

// Run a BAI sim at the current position with the given strategy. Populates
// sim_results, returns the best move (NOT played). The returned pointer is
// into sim_results' SimmedPlay array and is valid until the next sim run.
// If `verbose` is true, prints the per-candidate sim-results table to stdout.
static const Move *run_sim_only(Game *game, MoveList *move_list,
                                SimResults *sim_results, SimCtx **sim_ctx,
                                WinPct *win_pcts, ThreadControl *tc,
                                const FidelityLevel *strategy, int num_plies,
                                bool verbose, ErrorStack *error_stack) {
  // Generate candidate moves
  const MoveGenArgs gen_args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .thread_index = 0,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  generate_moves(&gen_args);

  const int num_moves = move_list_get_count(move_list);
  if (num_moves <= 1) {
    // Only one move (or pass)
    return move_list_get_move(move_list, 0);
  }

  // Build SimArgs with fixed time limit, no threshold
  SimArgs sim_args;
  sim_args_fill(num_plies, move_list,
                /* num_plays */ NUM_PLAYS,
                /* known_opp_rack */ NULL, win_pcts,
                /* inference_results */ NULL, tc, game,
                /* sim_with_inference */ false,
                /* use_heat_map */ false, NUM_THREADS,
                /* print_interval */ 0,
                /* max_num_display_plays */ NUM_PLAYS,
                /* max_num_display_plies */ num_plies,
                /* seed */ 0,
                /* max_iterations */ UINT64_MAX,
                /* min_play_iterations */ OUTER_MIN_PLAY_ITERATIONS,
                /* scond */ 101.0, // > 100 → BAI_THRESHOLD_NONE
                BAI_THRESHOLD_NONE, strategy->time_limit_seconds,
                BAI_SAMPLING_RULE_TOP_TWO_IDS,
                /* cutoff */ 0.0,
                /* inference_args */ NULL, &sim_args);
  sim_args.utility_w_winpct = OUTER_UTILITY_W_WINPCT;
  sim_args.utility_w_spread = OUTER_UTILITY_W_SPREAD;
  sim_args.utility_spread_scale = OUTER_UTILITY_SPREAD_SCALE;

  // For nested-leaf sims, turn off BAI's similarity-key pruning so that
  // same-square different-anagram arms ("epigons") each get sampled and
  // we get reliable per-arm wpcts.
  if (strategy->ply_strategy == PLY_STRATEGY_NESTED_SIM) {
    sim_args.bai_options.disable_similarity = true;
  }

  // Override fidelity to use our strategy
  sim_args.num_fidelity_levels = 1;
  sim_args.fidelity_levels[0] = *strategy;

  thread_control_set_status(tc, THREAD_CONTROL_STATUS_STARTED);
  simulate(&sim_args, sim_ctx, sim_results, error_stack);

  if (verbose) {
    char *sim_str =
        sim_results_get_string(game, sim_results, NUM_PLAYS, num_plies,
                               /*filter_row=*/-1, /*filter_col=*/-1,
                               /*prefix_mls=*/NULL, /*prefix_len=*/0,
                               /*exclude_tile_placement_moves=*/false,
                               /*use_ucgi_format=*/false,
                               /*game_board_string=*/NULL);
    printf("    --- Sim Results ---\n%s", sim_str);
    free(sim_str);
  }

  BAIResult *bai_result = sim_results_get_bai_result(sim_results);
  int best_arm = bai_result_get_best_arm(bai_result);
  if (best_arm < 0) {
    best_arm = 0;
  }

  // Per-turn outer-sim sample count log (smoke test diagnostics).
  const char *label = (strategy->ply_strategy == PLY_STRATEGY_NESTED_SIM)
                          ? "NESTED"
                          : "STATIC";
  const int num_arms = (num_moves < NUM_PLAYS) ? num_moves : NUM_PLAYS;
  uint64_t total_samples = 0;
  uint64_t best_samples = 0;
  uint64_t max_samples = 0;
  uint64_t min_samples = UINT64_MAX;
  for (int arm = 0; arm < num_arms; arm++) {
    const SimmedPlay *sp = sim_results_get_simmed_play(sim_results, arm);
    const uint64_t n = stat_get_num_samples(simmed_play_get_win_pct_stat(sp));
    total_samples += n;
    if (n > max_samples) {
      max_samples = n;
    }
    if (n < min_samples) {
      min_samples = n;
    }
    if (arm == best_arm) {
      best_samples = n;
    }
  }
  InnerDiag diag = {0};
  sim_ctx_get_inner_diag(*sim_ctx, &diag);
  const double avg_inner_rollouts =
      diag.calls > 0 ? (double)diag.rollouts / (double)diag.calls : 0.0;
  const double early_stop_rate =
      diag.calls > 0 ? (double)diag.early_stops / (double)diag.calls : 0.0;
  printf("    [outer %s] arms=%d total_samples=%" PRIu64 " best_arm=%d "
         "best_samples=%" PRIu64 " max=%" PRIu64 " min=%" PRIu64
         " | inner: calls=%" PRIu64 " avg_rollouts=%.1f early_stops=%" PRIu64
         " (%.1f%%)\n",
         label, num_arms, total_samples, best_arm, best_samples, max_samples,
         min_samples, diag.calls, avg_inner_rollouts, diag.early_stops,
         100.0 * early_stop_rate);

  return simmed_play_get_move(sim_results_get_simmed_play(sim_results, best_arm));
}

// Count tiles placed on the board (CSW21 has 100 total tiles in the bag at
// game start).
static int count_tiles_on_board(const Game *game) {
  const int bag_count = bag_get_letters(game_get_bag(game));
  const int rack_count =
      rack_get_total_letters(player_get_rack(game_get_player(game, 0))) +
      rack_get_total_letters(player_get_rack(game_get_player(game, 1)));
  return 100 - bag_count - rack_count;
}

// Play one move using BAI with the given fidelity level.
// Returns the move played (pointer into move_list, valid until next gen).
static const Move *play_sim_turn(Game *game, MoveList *move_list,
                                 SimResults *sim_results, SimCtx **sim_ctx,
                                 WinPct *win_pcts, ThreadControl *tc,
                                 const FidelityLevel *strategy, int num_plies,
                                 ErrorStack *error_stack) {
  const Move *best_move;

  // Late game (tiles_on_board > threshold): skip any nested sim and just play
  // a short flat sim to drive the game to completion.
  const int tiles_on_board = count_tiles_on_board(game);
  if (tiles_on_board > LATE_GAME_TILES_THRESHOLD) {
    FidelityLevel rush = STRATEGY_STATIC;
    rush.time_limit_seconds = LATE_GAME_FLAT_BUDGET_S;
    printf("    [late game: %d tiles on board → %.1fs flat rush]\n",
           tiles_on_board, LATE_GAME_FLAT_BUDGET_S);
    best_move = run_sim_only(game, move_list, sim_results, sim_ctx, win_pcts,
                             tc, &rush, num_plies, /*verbose=*/true,
                             error_stack);
    play_move(best_move, game, NULL);
    return best_move;
  }

  if (strategy == &STRATEGY_HYBRID) {
    // Phase 1: nested probe at HYBRID_PROBE_SECS to gauge spiciness.
    FidelityLevel probe = STRATEGY_NESTED;
    probe.time_limit_seconds = HYBRID_PROBE_SECS;
    const double total_budget = strategy->time_limit_seconds;
    const double cont_secs = total_budget - HYBRID_PROBE_SECS;
    (void)run_sim_only(game, move_list, sim_results, sim_ctx, win_pcts, tc,
                       &probe, num_plies, /*verbose=*/true, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return move_list_get_move(move_list, 0);
    }
    InnerDiag diag = {0};
    sim_ctx_get_inner_diag(*sim_ctx, &diag);
    const double mean_loss =
        diag.calls > 0 ? diag.loss_sum / (double)diag.calls : 0.0;
    const bool spicy = mean_loss > HYBRID_THRESHOLD;
    printf("    [hybrid] probe %.2fs mean_loss=%.4f thr=%.4f → %s %.2fs\n",
           HYBRID_PROBE_SECS, mean_loss, HYBRID_THRESHOLD,
           spicy ? "NESTED" : "FLAT", cont_secs);

    // Phase 2: continuation using either nested or flat for the remainder.
    FidelityLevel cont = spicy ? STRATEGY_NESTED : STRATEGY_STATIC;
    cont.time_limit_seconds = cont_secs;
    best_move = run_sim_only(game, move_list, sim_results, sim_ctx, win_pcts,
                             tc, &cont, num_plies, /*verbose=*/true,
                             error_stack);
  } else {
    best_move = run_sim_only(game, move_list, sim_results, sim_ctx, win_pcts,
                             tc, strategy, num_plies, /*verbose=*/true,
                             error_stack);
  }
  play_move(best_move, game, NULL);
  return best_move;
}

// Play a full game. p0_strategy and p1_strategy control fidelity per player.
// If history != NULL, the first MAX_TRACKED_TURNS played moves are copied in
// and *num_turns_out is set to the actual count.
static void play_game(Game *game, MoveList *move_list, SimResults *sim_results,
                      SimCtx **sim_ctx, WinPct *win_pcts, ThreadControl *tc,
                      EndgameCtx **ctx_ptr, EndgameResults *endgame_results,
                      const FidelityLevel *p0_strategy,
                      const FidelityLevel *p1_strategy, int p0_plies,
                      int p1_plies, const char *p0_label, const char *p1_label,
                      int game_num, uint64_t seed, PairResults *results,
                      Move *history, int *num_turns_out) {
  game_reset(game);
  game_seed(game, seed);
  draw_starting_racks(game);

  ErrorStack *error_stack = error_stack_create();
  StringBuilder *sb = string_builder_create();
  GameStringOptions *gso = game_string_options_create_default();
  struct timespec game_start, game_end;
  clock_gettime(CLOCK_MONOTONIC, &game_start);

  int turn = 0;
  while (!game_over(game)) {
    int player_idx = game_get_player_on_turn_index(game);
    const FidelityLevel *strategy =
        (player_idx == 0) ? p0_strategy : p1_strategy;
    int plies = (player_idx == 0) ? p0_plies : p1_plies;
    const char *label = (player_idx == 0) ? p0_label : p1_label;

    // Print board before each turn
    string_builder_clear(sb);
    string_builder_add_game(game, NULL, gso, NULL, sb);
    printf("    -- Before turn %d --\n%s\n", turn + 1, string_builder_peek(sb));

    int unseen = tiles_unseen(game);
    const Move *move;
    bool bag_empty = bag_is_empty(game_get_bag(game));

    struct timespec turn_start, turn_end;
    clock_gettime(CLOCK_MONOTONIC, &turn_start);

    if (bag_empty) {
      // Both players use endgame solver when bag is empty
      printf("    [turn %d: ENDGAME mode, unseen=%d]\n", turn + 1, unseen);
      move = play_endgame_turn(game, move_list, ctx_ptr, endgame_results, tc,
                               error_stack);
      label = "ENDGM";
    } else {
      int effective_plies = plies;
      if (unseen < LATE_GAME_TILE_THRESHOLD) {
        effective_plies = LATE_GAME_PLIES;
        printf("    [turn %d: LATE-GAME mode, unseen=%d, plies=%d→%d]\n",
               turn + 1, unseen, plies, effective_plies);
      } else {
        printf("    [turn %d: SIM mode, unseen=%d, plies=%d]\n", turn + 1,
               unseen, plies);
      }
      move = play_sim_turn(game, move_list, sim_results, sim_ctx, win_pcts, tc,
                           strategy, effective_plies, error_stack);
    }

    clock_gettime(CLOCK_MONOTONIC, &turn_end);
    double turn_elapsed = (turn_end.tv_sec - turn_start.tv_sec) +
                          (turn_end.tv_nsec - turn_start.tv_nsec) / 1e9;

    if (history && turn < MAX_TRACKED_TURNS) {
      history[turn] = *move; // shallow copy is fine; Move is POD
    }
    turn++;

    // Log the move played this turn
    string_builder_clear(sb);
    string_builder_add_move(sb, game_get_board(game), move, game_get_ld(game),
                            true);
    printf("    Turn %2d (%-6s): %s  [%.1fs]\n", turn, label,
           string_builder_peek(sb), turn_elapsed);

    if (!error_stack_is_empty(error_stack)) {
      printf("  ERROR on turn %d: ", turn);
      error_stack_print_and_reset(error_stack);
      break;
    }
  }
  if (num_turns_out) {
    *num_turns_out = turn;
  }

  // Print final board
  string_builder_clear(sb);
  string_builder_add_game(game, NULL, gso, NULL, sb);
  printf("%s\n", string_builder_peek(sb));
  game_string_options_destroy(gso);

  clock_gettime(CLOCK_MONOTONIC, &game_end);
  double elapsed = (game_end.tv_sec - game_start.tv_sec) +
                   (game_end.tv_nsec - game_start.tv_nsec) / 1e9;

  int p0_score = equity_to_int(player_get_score(game_get_player(game, 0)));
  int p1_score = equity_to_int(player_get_score(game_get_player(game, 1)));

  const char *winner_label;
  if (p0_score > p1_score) {
    results->p0_wins++;
    winner_label = p0_label;
  } else if (p1_score > p0_score) {
    results->p1_wins++;
    winner_label = p1_label;
  } else {
    results->ties++;
    winner_label = "TIE";
  }
  results->total_turns += turn;
  results->total_elapsed_s += elapsed;

  printf("  Game %2d: %s(%d) vs %s(%d) → %s  [%d turns, %.1fs]\n", game_num,
         p0_label, p0_score, p1_label, p1_score, winner_label, turn, elapsed);

  if (g_csv_file) {
    fprintf(g_csv_file, "%d,%" PRIu64 ",%s,%d,%s,%d,%s,%d,%.2f\n", game_num,
            seed, p0_label, p0_score, p1_label, p1_score, winner_label, turn,
            elapsed);
    fflush(g_csv_file);
  }

  string_builder_destroy(sb);
  error_stack_destroy(error_stack);
}

// At the first turn where the two paired games picked different moves, re-run
// both strategies at the (still-aligned) position and log cross-evaluation
// data: each strategy's pick (from its rerun), the other strategy's
// evaluation of that pick (win% + rank among 15 candidates), and position
// metadata. The rerun picks are used as the canonical "STATIC choice" and
// "NESTED choice" — independent of which player happened to be on turn —
// since the cross-eval lookups must be in each strategy's own sim_results.
//
// Sims are run with the same fidelity/budget as the in-game turn (no extra
// stopping condition beyond the time limit).
static void
analyze_pair_disagreement(int pair_num, uint64_t seed, Game *game,
                          MoveList *move_list, SimResults *sim_results,
                          SimCtx **sim_ctx, WinPct *win_pcts,
                          ThreadControl *tc, const Move *history_a,
                          int num_turns_a, const Move *history_b,
                          int num_turns_b) {
  int diverge_turn = -1;
  int min_turns = (num_turns_a < num_turns_b) ? num_turns_a : num_turns_b;
  for (int t = 0; t < min_turns; t++) {
    if (!moves_equivalent(&history_a[t], &history_b[t])) {
      diverge_turn = t;
      break;
    }
  }
  if (diverge_turn < 0) {
    return;
  }

  // Replay aligned moves to reconstruct the divergence position.
  game_reset(game);
  game_seed(game, seed);
  draw_starting_racks(game);
  for (int t = 0; t < diverge_turn; t++) {
    play_move(&history_a[t], game, NULL);
  }

  // Skip endgame positions — STRATEGY_NESTED isn't applicable there
  // (both strategies would defer to the endgame solver).
  if (bag_is_empty(game_get_bag(game))) {
    return;
  }

  const int unseen = tiles_unseen(game);
  const int late_game = (unseen < LATE_GAME_TILE_THRESHOLD);
  const int static_plies = late_game ? LATE_GAME_PLIES : STATIC_PLIES;
  const int nested_plies = late_game ? LATE_GAME_PLIES : NESTED_PLIES;
  const int on_turn_player = game_get_player_on_turn_index(game);

  ErrorStack *error_stack = error_stack_create();
  StringBuilder *sb = string_builder_create();

  // --- STATIC sim at the divergence position ---
  const Move *static_pick_ptr = run_sim_only(
      game, move_list, sim_results, sim_ctx, win_pcts, tc, &STRATEGY_STATIC,
      static_plies, /*verbose=*/false, error_stack);
  // Copy STATIC's pick before sim_results is overwritten by NESTED.
  Move static_pick = *static_pick_ptr;
  // Snapshot STATIC's 15 plays so we can look up NESTED's pick later.
  SimSnapshot static_snap;
  snapshot_sim_results(sim_results, NUM_PLAYS, &static_snap);

  string_builder_clear(sb);
  string_builder_add_move(sb, game_get_board(game), &static_pick,
                          game_get_ld(game), true);
  char *static_pick_str = string_duplicate(string_builder_peek(sb));

  // --- Run NESTED sim from the same position (no play_move was issued
  // by the STATIC sim, so game state is unchanged). ---
  const Move *nested_pick_ptr = run_sim_only(
      game, move_list, sim_results, sim_ctx, win_pcts, tc, &STRATEGY_NESTED,
      nested_plies, /*verbose=*/false, error_stack);
  Move nested_pick = *nested_pick_ptr;
  SimSnapshot nested_snap;
  snapshot_sim_results(sim_results, NUM_PLAYS, &nested_snap);

  string_builder_clear(sb);
  string_builder_add_move(sb, game_get_board(game), &nested_pick,
                          game_get_ld(game), true);
  char *nested_pick_str = string_duplicate(string_builder_peek(sb));

  // Cross-eval lookups.
  double static_pick_wpct = 0.0, nested_pick_wpct_in_static = 0.0;
  int static_pick_rank = -1, nested_pick_rank_in_static = -1;
  find_move_in_snapshot(&static_snap, &static_pick, &static_pick_wpct,
                        &static_pick_rank);
  find_move_in_snapshot(&static_snap, &nested_pick, &nested_pick_wpct_in_static,
                        &nested_pick_rank_in_static);

  double nested_pick_wpct = 0.0, static_pick_wpct_in_nested = 0.0;
  int nested_pick_rank = -1, static_pick_rank_in_nested = -1;
  find_move_in_snapshot(&nested_snap, &nested_pick, &nested_pick_wpct,
                        &nested_pick_rank);
  find_move_in_snapshot(&nested_snap, &static_pick, &static_pick_wpct_in_nested,
                        &static_pick_rank_in_nested);

  // Rack as text.
  string_builder_clear(sb);
  string_builder_add_rack(sb,
                          player_get_rack(game_get_player(game, on_turn_player)),
                          game_get_ld(game), false);
  char *rack_str = string_duplicate(string_builder_peek(sb));
  char *cgp = game_get_cgp(game, /*write_player_on_turn_first=*/false);

  fprintf(g_disagree_file,
          "%d,%" PRIu64 ",%d,%d,%s,%d,%d,\"%s\",\"%s\","
          "%.4f,%d,%.4f,%d,%.4f,%d,%.4f,%d,\"%s\"\n",
          pair_num, seed, diverge_turn + 1, on_turn_player, rack_str, unseen,
          late_game, static_pick_str, nested_pick_str, static_pick_wpct,
          static_pick_rank, nested_pick_wpct, nested_pick_rank,
          nested_pick_wpct_in_static, nested_pick_rank_in_static,
          static_pick_wpct_in_nested, static_pick_rank_in_nested, cgp);
  fflush(g_disagree_file);

  free(static_pick_str);
  free(nested_pick_str);
  free(rack_str);
  free(cgp);
  string_builder_destroy(sb);
  error_stack_destroy(error_stack);
}

void test_gamepair_bai_benchmark(void) {
  setbuf(stdout, NULL); // Disable stdout buffering for real-time output

  // Env-var overrides for A/B sweeps.
  int num_game_pairs = NUM_GAME_PAIRS;
  const char *env_pairs = getenv("GAMEPAIRBAI_PAIRS");
  if (env_pairs && *env_pairs) {
    num_game_pairs = atoi(env_pairs);
  }
  const char *env_k = getenv("GAMEPAIRBAI_K");
  if (env_k && *env_k) {
    STRATEGY_NESTED.nested_candidates = atoi(env_k);
  }
  const char *env_n = getenv("GAMEPAIRBAI_N");
  if (env_n && *env_n) {
    STRATEGY_NESTED.nested_rollouts = atoi(env_n);
  }
  const char *env_tlim = getenv("GAMEPAIRBAI_TLIM");
  if (env_tlim && *env_tlim) {
    const double tlim = atof(env_tlim);
    STRATEGY_STATIC.time_limit_seconds = tlim;
    STRATEGY_NESTED.time_limit_seconds = tlim;
  }
  const char *env_iww = getenv("GAMEPAIRBAI_INNER_W_WINPCT");
  if (env_iww && *env_iww) {
    STRATEGY_NESTED.inner_w_winpct = atof(env_iww);
  }
  const char *env_iws = getenv("GAMEPAIRBAI_INNER_W_SPREAD");
  if (env_iws && *env_iws) {
    STRATEGY_NESTED.inner_w_spread = atof(env_iws);
  }
  const char *env_iss = getenv("GAMEPAIRBAI_INNER_SPREAD_SCALE");
  if (env_iss && *env_iss) {
    STRATEGY_NESTED.inner_spread_scale = atof(env_iss);
  }
  const char *env_imax = getenv("GAMEPAIRBAI_INNER_MAX");
  if (env_imax && *env_imax) {
    STRATEGY_NESTED.nested_max_samples = atoi(env_imax);
  }
  const char *env_istopz = getenv("GAMEPAIRBAI_INNER_STOPZ");
  if (env_istopz && *env_istopz) {
    STRATEGY_NESTED.nested_stop_z = atof(env_istopz);
  }
  // Hybrid: when set, replaces STATIC with HYBRID in the matchup.
  // HYBRID spends GAMEPAIRBAI_HYBRID_PROBE seconds on a nested probe, then
  // continues with nested (if inner mean_loss > GAMEPAIRBAI_HYBRID_THRESHOLD)
  // or flat (otherwise) for the remaining time-budget.
  const bool hybrid_enabled =
      getenv("GAMEPAIRBAI_HYBRID") && *getenv("GAMEPAIRBAI_HYBRID");
  const char *env_hp = getenv("GAMEPAIRBAI_HYBRID_PROBE");
  if (env_hp && *env_hp) {
    HYBRID_PROBE_SECS = atof(env_hp);
  }
  const char *env_ht = getenv("GAMEPAIRBAI_HYBRID_THRESHOLD");
  if (env_ht && *env_ht) {
    HYBRID_THRESHOLD = atof(env_ht);
  }
  // Inherit the same tlim and inner params for the HYBRID strategy.
  STRATEGY_HYBRID.time_limit_seconds = STRATEGY_NESTED.time_limit_seconds;
  STRATEGY_HYBRID.nested_candidates = STRATEGY_NESTED.nested_candidates;
  STRATEGY_HYBRID.nested_rollouts = STRATEGY_NESTED.nested_rollouts;
  STRATEGY_HYBRID.nested_plies = STRATEGY_NESTED.nested_plies;
  STRATEGY_HYBRID.nested_max_samples = STRATEGY_NESTED.nested_max_samples;
  STRATEGY_HYBRID.nested_stop_z = STRATEGY_NESTED.nested_stop_z;
  STRATEGY_HYBRID.inner_w_winpct = STRATEGY_NESTED.inner_w_winpct;
  STRATEGY_HYBRID.inner_w_spread = STRATEGY_NESTED.inner_w_spread;
  STRATEGY_HYBRID.inner_spread_scale = STRATEGY_NESTED.inner_spread_scale;
  FidelityLevel *alt_strategy =
      hybrid_enabled ? &STRATEGY_HYBRID : &STRATEGY_STATIC;
  const char *alt_label = hybrid_enabled ? "HYBRID" : "STATIC";
  // Opponent of the ALT strategy. Default NESTED for backwards compat;
  // set GAMEPAIRBAI_OPP_FLAT=1 to make it STATIC (e.g., for HYBRID vs FLAT).
  FidelityLevel *opp_strategy = &STRATEGY_NESTED;
  const char *opp_label = "NESTED";
  if (getenv("GAMEPAIRBAI_OPP_FLAT") && *getenv("GAMEPAIRBAI_OPP_FLAT")) {
    opp_strategy = &STRATEGY_STATIC;
    opp_label = "STATIC";
  }
  const char *env_csv = getenv("GAMEPAIRBAI_CSV");
  if (env_csv && *env_csv) {
    g_csv_file = fopen(env_csv, "a");
    if (g_csv_file) {
      // Header (best-effort: written on every open; harmless when appending).
      fprintf(g_csv_file,
              "game_num,seed,p0_label,p0_score,p1_label,p1_score,winner,turns,"
              "wall_s\n");
      fflush(g_csv_file);
    }
  }
  const char *env_disagree = getenv("GAMEPAIRBAI_DISAGREE_CSV");
  if (env_disagree && *env_disagree) {
    g_disagree_file = fopen(env_disagree, "a");
    if (g_disagree_file) {
      fprintf(g_disagree_file,
              "pair,seed,turn,on_turn_player,rack,unseen,late_game,"
              "static_pick,nested_pick,"
              "static_pick_wpct,static_pick_rank,"
              "nested_pick_wpct,nested_pick_rank,"
              "nested_pick_wpct_in_static,nested_pick_rank_in_static,"
              "static_pick_wpct_in_nested,static_pick_rank_in_nested,"
              "cgp\n");
      fflush(g_disagree_file);
    }
  }

  printf("\n");
  printf("================================================\n");
  printf("  Game-Pair BAI Benchmark\n");
  printf("  %d game pairs, %.2fs/turn, %d threads\n", num_game_pairs,
         STRATEGY_STATIC.time_limit_seconds, NUM_THREADS);
  printf("  Matchup: %s vs %s%s\n", alt_label, opp_label,
         hybrid_enabled ? "" : "");
  if (hybrid_enabled) {
    printf("  HYBRID probe=%.2fs threshold=%.4f\n", HYBRID_PROBE_SECS,
           HYBRID_THRESHOLD);
  }
  printf("  Inner TOP_TWO: floor=%d/arm, max=%d total, stop_z=%.3f\n",
         STRATEGY_NESTED.nested_rollouts, STRATEGY_NESTED.nested_max_samples,
         STRATEGY_NESTED.nested_stop_z);
  printf("  Static(%d-ply) vs Nested(%d-ply outer, %d-ply inner, K=%d N=%d)\n",
         STATIC_PLIES, NESTED_PLIES, STRATEGY_NESTED.nested_plies,
         STRATEGY_NESTED.nested_candidates, STRATEGY_NESTED.nested_rollouts);
  printf("  Inner blend: w_winpct=%.2f w_spread=%.2f spread_scale=%.1f\n",
         STRATEGY_NESTED.inner_w_winpct, STRATEGY_NESTED.inner_w_spread,
         STRATEGY_NESTED.inner_spread_scale);
  printf("  Endgame: %d-ply, %.0fs limit | Late-game: <%d unseen → %d plies\n",
         ENDGAME_PLIES, ENDGAME_TIME_LIMIT_S, LATE_GAME_TILE_THRESHOLD,
         LATE_GAME_PLIES);
  printf("================================================\n");

  // Create config and load game data via a CGP
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -rit true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 15 -plies 2 -threads 10");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);

  // Load win_pcts directly (avoid throwaway sim on empty board)
  ErrorStack *load_es = error_stack_create();
  WinPct *win_pcts =
      win_pct_create(config_get_data_paths(config), DEFAULT_WIN_PCT, load_es);
  if (!error_stack_is_empty(load_es)) {
    error_stack_print_and_reset(load_es);
    error_stack_destroy(load_es);
    config_destroy(config);
    return;
  }
  error_stack_destroy(load_es);

  ThreadControl *tc = config_get_thread_control(config);

  // Create game resources
  Game *game = game_duplicate(config_get_game(config));
  MoveList *move_list = move_list_create(NUM_PLAYS);
  SimResults *sim_results = sim_results_create(0.0);
  SimCtx *sim_ctx = NULL;
  EndgameCtx *endgame_ctx = NULL;
  EndgameResults *endgame_results = endgame_results_create();

  PairResults static_as_p0 = {0}; // static=P0, nested=P1
  PairResults nested_as_p0 = {0}; // nested=P0, static=P1

  // Per-pair move histories used by analyze_pair_disagreement.
  Move *history_a = malloc_or_die(sizeof(Move) * MAX_TRACKED_TURNS);
  Move *history_b = malloc_or_die(sizeof(Move) * MAX_TRACKED_TURNS);

  printf("\n--- Games ---\n");
  for (int pair = 0; pair < num_game_pairs; pair++) {
    uint64_t seed = 9 + (uint64_t)pair;
    int num_turns_a = 0;
    int num_turns_b = 0;

    // Game A: ALT=P0, OPP=P1
    play_game(game, move_list, sim_results, &sim_ctx, win_pcts, tc,
              &endgame_ctx, endgame_results, alt_strategy, opp_strategy,
              STATIC_PLIES, NESTED_PLIES, alt_label, opp_label, pair * 2 + 1,
              seed, &static_as_p0, history_a, &num_turns_a);

    // Game B: OPP=P0, ALT=P1 (same seed, swapped strategies)
    play_game(game, move_list, sim_results, &sim_ctx, win_pcts, tc,
              &endgame_ctx, endgame_results, opp_strategy, alt_strategy,
              NESTED_PLIES, STATIC_PLIES, opp_label, alt_label, pair * 2 + 2,
              seed, &nested_as_p0, history_b, &num_turns_b);

    if (g_disagree_file) {
      analyze_pair_disagreement(pair + 1, seed, game, move_list, sim_results,
                                &sim_ctx, win_pcts, tc, history_a, num_turns_a,
                                history_b, num_turns_b);
    }
  }
  free(history_a);
  free(history_b);

  // Aggregate: count wins by strategy, not by player position
  int static_wins = static_as_p0.p0_wins + nested_as_p0.p1_wins;
  int nested_wins = static_as_p0.p1_wins + nested_as_p0.p0_wins;
  int ties = static_as_p0.ties + nested_as_p0.ties;
  int total_games = num_game_pairs * 2;
  int total_turns = static_as_p0.total_turns + nested_as_p0.total_turns;
  double total_elapsed =
      static_as_p0.total_elapsed_s + nested_as_p0.total_elapsed_s;

  printf("\n================================================\n");
  printf("  RESULTS (%d games = %d pairs)\n", total_games, num_game_pairs);
  printf("  %s wins: %d (%.1f%%)\n", alt_label, static_wins,
         100.0 * static_wins / total_games);
  printf("  %s wins: %d (%.1f%%)\n", opp_label, nested_wins,
         100.0 * nested_wins / total_games);
  printf("  Ties:        %d\n", ties);
  printf("  Avg turns/game: %.1f\n", (double)total_turns / total_games);
  printf("  Total wall time: %.1fs (%.1fs/game avg)\n", total_elapsed,
         total_elapsed / total_games);
  printf("================================================\n");

  endgame_results_destroy(endgame_results);
  endgame_ctx_destroy(endgame_ctx);
  sim_ctx_destroy(sim_ctx);
  sim_results_destroy(sim_results);
  move_list_destroy(move_list);
  game_destroy(game);
  win_pct_destroy(win_pcts);
  config_destroy(config);
  if (g_csv_file) {
    fclose(g_csv_file);
    g_csv_file = NULL;
  }
  if (g_disagree_file) {
    fclose(g_disagree_file);
    g_disagree_file = NULL;
  }
}

// ---------------------------------------------------------------------------
// NESTED vs NESTED convergence smoke test
// ---------------------------------------------------------------------------
// For each turn, while the per-turn sim runs, a poller thread reads each
// candidate's win_pct_stat mean once per second and records argmax (the
// "current best arm" by win%). Output shows how quickly BAI's pick
// stabilizes and how often it flips during the time budget.
//
// Reads of stat_get_mean() during the sim are not locked, but they're
// integer/double reads of a value that's updated infrequently per-arm —
// tolerable for polling. BAI's official `best_arm` field is only written at
// the very end of the sim so it isn't useful as a live signal.

#define MAX_CONV_SNAPS 60

typedef struct ConvPollerArgs {
  SimResults *sim_results;
  int num_candidates;
  // Output: snap_arm[i] = argmax-by-wpct at second (i+1). Capacity max_snaps.
  int *snap_arm;
  int max_snaps;
  int snap_count;
  volatile bool done;
} ConvPollerArgs;

static void *conv_poller_thread(void *arg) {
  ConvPollerArgs *pa = arg;
  while (!pa->done && pa->snap_count < pa->max_snaps) {
    struct timespec ts = {.tv_sec = 1, .tv_nsec = 0};
    nanosleep(&ts, NULL);
    if (pa->done) {
      break;
    }
    int best = -1;
    double best_mean = -1.0;
    for (int i = 0; i < pa->num_candidates; i++) {
      const SimmedPlay *sp = sim_results_get_simmed_play(pa->sim_results, i);
      double mean = stat_get_mean(simmed_play_get_win_pct_stat(sp));
      if (mean > best_mean) {
        best_mean = mean;
        best = i;
      }
    }
    pa->snap_arm[pa->snap_count++] = best;
  }
  return NULL;
}

static const Move *
play_turn_with_convergence(Game *game, MoveList *move_list,
                           SimResults *sim_results, SimCtx **sim_ctx,
                           WinPct *win_pcts, ThreadControl *tc,
                           const FidelityLevel *strategy, int num_plies,
                           int game_num, int turn_num,
                           ErrorStack *error_stack) {
  const MoveGenArgs gen_args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .thread_index = 0,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  generate_moves(&gen_args);
  const int num_moves = move_list_get_count(move_list);
  if (num_moves <= 1) {
    const Move *only = move_list_get_move(move_list, 0);
    play_move(only, game, NULL);
    return only;
  }

  SimArgs sim_args;
  sim_args_fill(num_plies, move_list,
                /* num_plays */ NUM_PLAYS,
                /* known_opp_rack */ NULL, win_pcts,
                /* inference_results */ NULL, tc, game,
                /* sim_with_inference */ false,
                /* use_heat_map */ false, NUM_THREADS,
                /* print_interval */ 0,
                /* max_num_display_plays */ NUM_PLAYS,
                /* max_num_display_plies */ num_plies,
                /* seed */ 0,
                /* max_iterations */ UINT64_MAX,
                /* min_play_iterations */ OUTER_MIN_PLAY_ITERATIONS,
                /* scond */ 101.0, BAI_THRESHOLD_NONE,
                strategy->time_limit_seconds, BAI_SAMPLING_RULE_TOP_TWO_IDS,
                /* cutoff */ 0.0,
                /* inference_args */ NULL, &sim_args);
  sim_args.utility_w_winpct = OUTER_UTILITY_W_WINPCT;
  sim_args.utility_w_spread = OUTER_UTILITY_W_SPREAD;
  sim_args.utility_spread_scale = OUTER_UTILITY_SPREAD_SCALE;
  sim_args.num_fidelity_levels = 1;
  sim_args.fidelity_levels[0] = *strategy;

  const int cap = (NUM_PLAYS < num_moves) ? NUM_PLAYS : num_moves;
  ConvPollerArgs pa = {
      .sim_results = sim_results,
      .num_candidates = cap,
      .snap_arm = malloc_or_die(sizeof(int) * MAX_CONV_SNAPS),
      .max_snaps = MAX_CONV_SNAPS,
      .snap_count = 0,
      .done = false,
  };
  pthread_t poller_tid;
  pthread_create(&poller_tid, NULL, conv_poller_thread, &pa);

  thread_control_set_status(tc, THREAD_CONTROL_STATUS_STARTED);
  simulate(&sim_args, sim_ctx, sim_results, error_stack);

  pa.done = true;
  pthread_join(poller_tid, NULL);

  int final_arm = bai_result_get_best_arm(sim_results_get_bai_result(sim_results));
  if (final_arm < 0) {
    final_arm = 0;
  }
  const Move *final_move =
      simmed_play_get_move(sim_results_get_simmed_play(sim_results, final_arm));

  // Print trajectory.
  StringBuilder *sb = string_builder_create();
  printf("  game %d turn %d (%d candidates, %.1fs budget):\n", game_num,
         turn_num, cap, strategy->time_limit_seconds);
  int changes = 0;
  int first_stable_t = -1;
  for (int i = 0; i < pa.snap_count; i++) {
    int arm = pa.snap_arm[i];
    const Move *m = (arm >= 0) ? simmed_play_get_move(
                                     sim_results_get_simmed_play(sim_results,
                                                                 arm))
                               : NULL;
    string_builder_clear(sb);
    if (m) {
      string_builder_add_move(sb, game_get_board(game), m, game_get_ld(game),
                              true);
    } else {
      string_builder_add_string(sb, "(none)");
    }
    bool changed = (i > 0 && pa.snap_arm[i] != pa.snap_arm[i - 1]);
    if (changed) {
      changes++;
      first_stable_t = -1;
    } else if (i > 0 && first_stable_t < 0) {
      first_stable_t = i; // started counting stable from this snapshot
    }
    printf("    t=%2ds  arm=%2d  %-20s%s\n", i + 1, arm,
           string_builder_peek(sb), changed ? "  <-- changed" : "");
  }
  // Compare final BAI pick to last polled pick.
  bool final_matches_last =
      (pa.snap_count > 0 && pa.snap_arm[pa.snap_count - 1] == final_arm);
  string_builder_clear(sb);
  string_builder_add_move(sb, game_get_board(game), final_move,
                          game_get_ld(game), true);
  printf("    FINAL pick: arm=%d  %s   (%d change%s in poll, last poll %s "
         "final)\n",
         final_arm, string_builder_peek(sb), changes,
         changes == 1 ? "" : "s", final_matches_last ? "==" : "!=");
  string_builder_destroy(sb);
  free(pa.snap_arm);

  play_move(final_move, game, NULL);
  return final_move;
}

void test_nested_convergence(void) {
  setbuf(stdout, NULL);

  int num_games = 5;
  const char *env_g = getenv("NESTCONV_GAMES");
  if (env_g && *env_g) {
    num_games = atoi(env_g);
  }
  double tlim = 10.0;
  const char *env_t = getenv("NESTCONV_TLIM");
  if (env_t && *env_t) {
    tlim = atof(env_t);
  }
  int kk = 7, nn = 13;
  const char *env_k = getenv("NESTCONV_K");
  if (env_k && *env_k) {
    kk = atoi(env_k);
  }
  const char *env_n = getenv("NESTCONV_N");
  if (env_n && *env_n) {
    nn = atoi(env_n);
  }

  FidelityLevel nested = STRATEGY_NESTED;
  nested.time_limit_seconds = tlim;
  nested.nested_candidates = kk;
  nested.nested_rollouts = nn;

  printf("\n");
  printf("================================================\n");
  printf("  Nested vs Nested convergence smoke test\n");
  printf("  %d games, %.2fs/turn, K=%d N=%d nested_plies=%d\n", num_games, tlim,
         kk, nn, nested.nested_plies);
  printf("  Inner blend: w_winpct=%.2f w_spread=%.2f scale=%.1f\n",
         nested.inner_w_winpct, nested.inner_w_spread,
         nested.inner_spread_scale);
  printf("================================================\n");

  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -rit true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 15 -plies 2 -threads 10");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  ErrorStack *load_es = error_stack_create();
  WinPct *win_pcts =
      win_pct_create(config_get_data_paths(config), DEFAULT_WIN_PCT, load_es);
  if (!error_stack_is_empty(load_es)) {
    error_stack_print_and_reset(load_es);
    error_stack_destroy(load_es);
    config_destroy(config);
    return;
  }
  error_stack_destroy(load_es);

  ThreadControl *tc = config_get_thread_control(config);
  Game *game = game_duplicate(config_get_game(config));
  MoveList *move_list = move_list_create(NUM_PLAYS);
  SimResults *sim_results = sim_results_create(0.0);
  SimCtx *sim_ctx = NULL;
  EndgameCtx *endgame_ctx = NULL;
  EndgameResults *endgame_results = endgame_results_create();
  ErrorStack *error_stack = error_stack_create();

  for (int g = 0; g < num_games; g++) {
    uint64_t seed = 9 + (uint64_t)g;
    printf("\n=== Game %d (seed %" PRIu64 ") ===\n", g + 1, seed);
    game_reset(game);
    game_seed(game, seed);
    draw_starting_racks(game);
    int turn = 0;
    while (!game_over(game)) {
      bool bag_empty = bag_is_empty(game_get_bag(game));
      if (bag_empty) {
        // Use endgame solver — not interesting for convergence study.
        const Move *m = play_endgame_turn(game, move_list, &endgame_ctx,
                                          endgame_results, tc, error_stack);
        (void)m;
        turn++;
        printf("  game %d turn %d: ENDGAME (skipping convergence trace)\n",
               g + 1, turn);
        continue;
      }
      int unseen = tiles_unseen(game);
      int plies = (unseen < LATE_GAME_TILE_THRESHOLD) ? LATE_GAME_PLIES
                                                       : NESTED_PLIES;
      turn++;
      play_turn_with_convergence(game, move_list, sim_results, &sim_ctx,
                                 win_pcts, tc, &nested, plies, g + 1, turn,
                                 error_stack);
      if (!error_stack_is_empty(error_stack)) {
        printf("  error on turn %d: ", turn);
        error_stack_print_and_reset(error_stack);
        break;
      }
    }
    int p0 = equity_to_int(player_get_score(game_get_player(game, 0)));
    int p1 = equity_to_int(player_get_score(game_get_player(game, 1)));
    printf("  Final: P1=%d P2=%d (%d turns)\n", p0, p1, turn);
  }

  endgame_results_destroy(endgame_results);
  endgame_ctx_destroy(endgame_ctx);
  sim_ctx_destroy(sim_ctx);
  sim_results_destroy(sim_results);
  move_list_destroy(move_list);
  game_destroy(game);
  win_pct_destroy(win_pcts);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

// Late-game position from seed 9, Game 1 (turn 18, unseen=19).
// On-turn player listed first (was P1/NESTED in original game).
#define LATE_GAME_TEST_CGP                                                     \
  "15/1G3NIQAB5/1A7R5/1ZEL1AGILItY3/PORISM3COOED1/1N2HUIA1K5/2WHO10/"          \
  "1TOMENTA2W1FEU/VOX4TAPERER1/3G6NERAL/3UNSUITED4/3V11/15/15/15 "             \
  "ACEFNOT/BEEIRST 306/324 0 -lex CSW21;"

// Pair 11 turn 2: CEIMRTZ on turn after PURELY at 8G, trailing 0-30.
#define PAIR11_TURN2_CGP                                                       \
  "15/15/15/15/15/15/15/6PURELY3/15/15/15/15/15/15/15 "                        \
  "CEIMRTZ/AEEFGSU 0/30 0 -lex CSW21;"

// Pair 23 turn 18: EINNOTW trailing 305-340, 20 tiles unseen (13 in bag).
// Late-game position that triggered premature win percentage cutoff with
// only 1 iteration per candidate in 99-ply sims.
#define PAIR23_TURN18_CGP                                                      \
  "B2VAGUS7/IF1O1I3C5/BO1CHEQUER5/1L1E1DIGLOT4/PEHS5OY4/AYE6NED3/"             \
  "T1R1DUX4I3/1MOKIhIS3L3/2I7PA3/2ZA6AT3/1GEE6WE3/1ISO7R3/1F1N11/"             \
  "1T13/15 DENORU?/EINNOTW 340/305 1 -lex CSW21;"

void test_gamepair_bai_late_game(void) {
  setbuf(stdout, NULL);
  printf("\n");
  printf("================================================\n");
  printf("  Single-Position Sim Comparison (Round Robin)\n");
  printf("  Static(%d-ply) vs Nested(%d-ply), %ds/turn, %d threads\n",
         STATIC_PLIES, NESTED_PLIES, TURN_TIME_LIMIT_S, NUM_THREADS);
  printf("================================================\n");

  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -rit true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 15 -plies 2 -threads 10");
  load_and_exec_config_or_die(config, "cgp " PAIR11_TURN2_CGP);

  ErrorStack *load_es = error_stack_create();
  WinPct *win_pcts =
      win_pct_create(config_get_data_paths(config), DEFAULT_WIN_PCT, load_es);
  if (!error_stack_is_empty(load_es)) {
    error_stack_print_and_reset(load_es);
    error_stack_destroy(load_es);
    config_destroy(config);
    return;
  }
  error_stack_destroy(load_es);

  ThreadControl *tc = config_get_thread_control(config);
  Game *game = game_duplicate(config_get_game(config));
  Game *game_copy_for_reset = game_duplicate(game);
  MoveList *move_list = move_list_create(NUM_PLAYS);
  SimResults *sim_results = sim_results_create(0.0);
  SimCtx *sim_ctx = NULL;
  ErrorStack *error_stack = error_stack_create();

  // Print the board
  StringBuilder *sb = string_builder_create();
  GameStringOptions *gso = game_string_options_create_default();
  string_builder_add_game(game, NULL, gso, NULL, sb);
  printf("\n%s\n", string_builder_peek(sb));
  printf("  Unseen tiles: %d\n", tiles_unseen(game));

  // Run STATIC strategy
  printf("\n--- Running STATIC sim (%d plies, %ds) ---\n", STATIC_PLIES,
         TURN_TIME_LIMIT_S);
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);

  const Move *move =
      play_sim_turn(game, move_list, sim_results, &sim_ctx, win_pcts, tc,
                    &STRATEGY_STATIC, STATIC_PLIES, error_stack);

  clock_gettime(CLOCK_MONOTONIC, &t1);
  double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

  string_builder_clear(sb);
  string_builder_add_move(sb, game_get_board(game), move, game_get_ld(game),
                          true);
  printf("\n  STATIC best: %s  [%.1fs]\n", string_builder_peek(sb), elapsed);

  // Reset game state for second run
  game_copy(game, game_copy_for_reset);

  // Run NESTED strategy
  printf("\n--- Running NESTED sim (%d plies, %ds) ---\n", NESTED_PLIES,
         TURN_TIME_LIMIT_S);
  clock_gettime(CLOCK_MONOTONIC, &t0);

  move = play_sim_turn(game, move_list, sim_results, &sim_ctx, win_pcts, tc,
                       &STRATEGY_NESTED, NESTED_PLIES, error_stack);

  clock_gettime(CLOCK_MONOTONIC, &t1);
  elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

  string_builder_clear(sb);
  string_builder_add_move(sb, game_get_board(game), move, game_get_ld(game),
                          true);
  printf("\n  NESTED best: %s  [%.1fs]\n", string_builder_peek(sb), elapsed);

  if (!error_stack_is_empty(error_stack)) {
    printf("  ERROR: ");
    error_stack_print_and_reset(error_stack);
  }

  game_string_options_destroy(gso);
  string_builder_destroy(sb);
  error_stack_destroy(error_stack);
  sim_ctx_destroy(sim_ctx);
  sim_results_destroy(sim_results);
  move_list_destroy(move_list);
  game_destroy(game_copy_for_reset);
  game_destroy(game);
  win_pct_destroy(win_pcts);
  config_destroy(config);
}

void test_gamepair_bai_cutoff(void) {
  setbuf(stdout, NULL);
  printf("\n");
  printf("================================================\n");
  printf("  Win Pct Cutoff Test (Pair 23 Turn 18)\n");
  printf("  Late-game 99-ply sim, %ds/turn, %d threads\n", TURN_TIME_LIMIT_S,
         NUM_THREADS);
  printf("  Verifies cutoff=0 does not stop sim early\n");
  printf("================================================\n");

  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -rit true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 15 -plies 2 -threads 10");
  load_and_exec_config_or_die(config, "cgp " PAIR23_TURN18_CGP);

  ErrorStack *load_es = error_stack_create();
  WinPct *win_pcts =
      win_pct_create(config_get_data_paths(config), DEFAULT_WIN_PCT, load_es);
  if (!error_stack_is_empty(load_es)) {
    error_stack_print_and_reset(load_es);
    error_stack_destroy(load_es);
    config_destroy(config);
    return;
  }
  error_stack_destroy(load_es);

  ThreadControl *tc = config_get_thread_control(config);
  Game *game = game_duplicate(config_get_game(config));
  MoveList *move_list = move_list_create(NUM_PLAYS);
  SimResults *sim_results = sim_results_create(0.0);
  SimCtx *sim_ctx = NULL;
  ErrorStack *error_stack = error_stack_create();

  // Print the board
  StringBuilder *sb = string_builder_create();
  GameStringOptions *gso = game_string_options_create_default();
  string_builder_add_game(game, NULL, gso, NULL, sb);
  printf("\n%s\n", string_builder_peek(sb));
  printf("  Unseen tiles: %d\n", tiles_unseen(game));

  // This position has 20 unseen tiles -> late-game 99-ply sims.
  // With the old cutoff bug, the sim would stop after 1 iteration per
  // candidate (seeing 100%/0% from single samples) and take <1s.
  // After the fix, it should run for the full time limit.
  printf("\n--- Running STATIC sim (99 plies, %ds) ---\n", TURN_TIME_LIMIT_S);
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);

  const Move *move =
      play_sim_turn(game, move_list, sim_results, &sim_ctx, win_pcts, tc,
                    &STRATEGY_STATIC, LATE_GAME_PLIES, error_stack);

  clock_gettime(CLOCK_MONOTONIC, &t1);
  double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

  string_builder_clear(sb);
  string_builder_add_move(sb, game_get_board(game), move, game_get_ld(game),
                          true);
  printf("\n  STATIC best: %s  [%.1fs]\n", string_builder_peek(sb), elapsed);

  // Verify the sim actually ran for a reasonable time (not cut off early)
  if (elapsed < 5.0) {
    printf("  WARNING: Sim completed in %.1fs — possible premature cutoff!\n",
           elapsed);
  } else {
    printf("  OK: Sim ran for %.1fs (no premature cutoff)\n", elapsed);
  }

  if (!error_stack_is_empty(error_stack)) {
    printf("  ERROR: ");
    error_stack_print_and_reset(error_stack);
  }

  game_string_options_destroy(gso);
  string_builder_destroy(sb);
  error_stack_destroy(error_stack);
  sim_ctx_destroy(sim_ctx);
  sim_results_destroy(sim_results);
  move_list_destroy(move_list);
  game_destroy(game);
  win_pct_destroy(win_pcts);
  config_destroy(config);
}
