// Simmed-inference benchmark: SIM vs SIMMEDINF game pairs.
//
// Usage: ./bin/magpie_test simmedinf
//
// Two player types compete in game pairs at the same total per-turn budget
// (SIM_BUDGET_S) on NUM_THREADS threads:
//   SIM        — 4-ply simulation, uniform rack sampling (no inference).
//   SIMMEDINF  — 4-ply simulation + simmed inference.
//                Before each sim turn, runs simmed_infer() on the opponent's
//                previous move to build a weighted leave distribution.
//                The outer sim then uses this precomputed distribution instead
//                of re-running static inference.
//
// Time budget per turn (approximate):
//   SIM:        SIM_BUDGET_S seconds of outer sim.
//   SIMMEDINF:  up to SIMMED_INFER_BUDGET_S seconds for simmed inference,
//               then SIM_BUDGET_S minus the inference's actual elapsed time
//               for the outer sim (so both arms get the same total).
//
// Results are printed after every game.

#include "../src/def/bai_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/bai_result.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/inference_args.h"
#include "../src/ent/inference_results.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/sim_args.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/win_pct.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/simmed_inference.h"
#include "../src/impl/simmer.h"
#include "../src/str/move_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_constants.h"
#include "test_util.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

// ── Configuration ──────────────────────────────────────────────────────────

#define NUM_SEEDS 200
#define NUM_PLAYS 15
#define NUM_PLIES 4
#define NUM_THREADS 10

// Outer sim time budget (seconds)
#define SIM_BUDGET_S 30.0

// Simmed-inference time budget (seconds).  Must leave enough time for outer
// sim.  The outer sim receives at least MIN_SIM_BUDGET_S seconds.
#define SIMMED_INFER_BUDGET_S 10.0
#define MIN_SIM_BUDGET_S 5.0

// Inner sim parameters (used by simmed_infer).
// INNER_NUM_PLAYS: top-N static-equity candidates generated per rack.
// simmed_inference.c always appends the observed move if it isn't in those N,
// giving ≤ N+1 arms.  Probe/full iterations are divided among those arms.
#define INNER_NUM_PLAYS 5
#define INNER_SIM_PLIES 2
#define INNER_PROBE_ITERS 120
#define INNER_FULL_ITERS 600
#define INNER_SIM_EQUITY_MARGIN 3.0

// Late-game: switch to many-ply sims when few tiles unseen
#define LATE_GAME_TILE_THRESHOLD 14
#define LATE_GAME_PLIES 99

// Log file
#define LOG_FILENAME "simmedinf_vs_sim4_log.csv"

// ── Player identity ────────────────────────────────────────────────────────

typedef enum {
  PLAYER_NOINF,     // sim, uniform rack sampling (no inference)
  PLAYER_STATICINF, // sim + static (KLV leave-value) inference
  PLAYER_SIMMEDINF, // sim + simmed inference (precomputed distribution)
  NUM_PLAYER_TYPES,
} player_type_t;

static const char *player_label(player_type_t type) {
  switch (type) {
  case PLAYER_NOINF:
    return "NOINF";
  case PLAYER_STATICINF:
    return "STATINF";
  case PLAYER_SIMMEDINF:
    return "SIMINF";
  default:
    return "?????";
  }
}

// Static inference accept window (display points): mirrors mainline's
// default -ima 5 for sims with inference.
#define STATIC_INFER_EQ_MARGIN 5

// ── Round-robin results ────────────────────────────────────────────────────
//
// wins[i][j] = games player type i won against player type j. Ties and the
// summed spread (i's score minus j's, over their games) are stored in the
// upper triangle (i < j).

typedef struct {
  int wins[NUM_PLAYER_TYPES][NUM_PLAYER_TYPES];
  int ties[NUM_PLAYER_TYPES][NUM_PLAYER_TYPES];
  long spread[NUM_PLAYER_TYPES][NUM_PLAYER_TYPES];
  int games[NUM_PLAYER_TYPES][NUM_PLAYER_TYPES];
} MatchResults;

// ── Timer thread (fires USER_INTERRUPT after a delay) ──────────────────────

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

// ── Elapsed time helper ────────────────────────────────────────────────────

static double timespec_diff_s(const struct timespec *start,
                              const struct timespec *end) {
  return (double)(end->tv_sec - start->tv_sec) +
         (double)(end->tv_nsec - start->tv_nsec) / 1e9;
}

// ── Tiles unseen by the player on turn ────────────────────────────────────

static int tiles_unseen(const Game *game) {
  return bag_get_letters(game_get_bag(game)) +
         rack_get_total_letters(player_get_rack(
             game_get_player(game, 1 - game_get_player_on_turn_index(game))));
}

// ── Build target_played_tiles from a tile-placement move ──────────────────

static void extract_played_tiles(const Move *move, int ld_size,
                                 Rack *target_played_tiles) {
  rack_set_dist_size_and_reset(target_played_tiles, ld_size);
  const int tiles_length = move_get_tiles_length(move);
  for (int i = 0; i < tiles_length; i++) {
    const MachineLetter ml = move_get_tile(move, i);
    if (ml != PLAYED_THROUGH_MARKER) {
      rack_add_letter(target_played_tiles,
                      get_is_blanked(ml) ? BLANK_MACHINE_LETTER : ml);
    }
  }
}

// ── Previous-move inference setup ──────────────────────────────────────────
//
// Rack storage plus the InferenceArgs that reference it, for inferring the
// opponent's previous move. The setup must outlive any simmed_infer() or
// simulate() call that uses its args (the args hold pointers into it).

typedef struct {
  Rack target_played_tiles;
  Rack target_known_rack;
  Rack nontarget_known_rack;
  InferenceArgs args;
} PrevMoveInferSetup;

static void fill_prev_move_infer_args(PrevMoveInferSetup *setup,
                                      const Game *game_before_prev,
                                      const Move *prev_move,
                                      int prev_player_index,
                                      Equity equity_margin, ThreadControl *tc) {
  const LetterDistribution *ld = game_get_ld(game_before_prev);
  const int ld_size = ld_get_size(ld);

  rack_set_dist_size_and_reset(&setup->target_known_rack, ld_size);
  rack_set_dist_size_and_reset(&setup->nontarget_known_rack, ld_size);

  int target_num_exch = 0;
  if (move_get_type(prev_move) == GAME_EVENT_EXCHANGE) {
    target_num_exch = move_get_tiles_played(prev_move);
    rack_set_dist_size_and_reset(&setup->target_played_tiles, ld_size);
  } else {
    extract_played_tiles(prev_move, ld_size, &setup->target_played_tiles);
  }

  // Our rack (nontarget) is known to us.
  const int nontarget_index = 1 - prev_player_index;
  rack_copy(
      &setup->nontarget_known_rack,
      player_get_rack(game_get_player(game_before_prev, nontarget_index)));

  infer_args_fill(&setup->args, NUM_PLAYS, equity_margin, NULL,
                  game_before_prev, NUM_THREADS,
                  /*parent_worker_thread_index=*/0,
                  /*print_interval=*/0, tc,
                  /*use_game_history=*/false,
                  /*use_inference_cutoff_optimization=*/true, prev_player_index,
                  move_get_score(prev_move), target_num_exch,
                  &setup->target_played_tiles, &setup->target_known_rack,
                  &setup->nontarget_known_rack);
}

// ── Run simmed inference on the opponent's previous move ──────────────────
//
// Populates inference_results with a weighted leave distribution based on
// inner sims. Returns the elapsed time and whether it completed (vs interrupt).

static bool run_simmed_inference(const Game *game_before_prev, WinPct *win_pcts,
                                 ThreadControl *tc, const Move *prev_move,
                                 int prev_player_index,
                                 InferenceResults *inference_results,
                                 ErrorStack *error_stack, double *elapsed_out) {
  PrevMoveInferSetup setup;
  fill_prev_move_infer_args(&setup, game_before_prev, prev_move,
                            prev_player_index, int_to_equity(0), tc);

  SimmedInferenceArgs si_args = {
      .base = &setup.args,
      .observed_move = prev_move,
      .win_pcts = win_pcts,
      .num_candidate_plays = INNER_NUM_PLAYS,
      .num_inner_sim_plies = INNER_SIM_PLIES,
      .probe_iterations = INNER_PROBE_ITERS,
      .full_iterations = INNER_FULL_ITERS,
      .time_budget_s = SIMMED_INFER_BUDGET_S,
      .sim_equity_margin = INNER_SIM_EQUITY_MARGIN,
  };

  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);

  thread_control_set_status(tc, THREAD_CONTROL_STATUS_STARTED);

  TimerArgs ta = {.tc = tc, .seconds = SIMMED_INFER_BUDGET_S, .done = false};
  pthread_t timer_tid;
  pthread_create(&timer_tid, NULL, timer_thread_func, &ta);

  simmed_infer(&si_args, inference_results, error_stack);

  ta.done = true;
  pthread_join(timer_tid, NULL);

  clock_gettime(CLOCK_MONOTONIC, &t1);
  *elapsed_out = timespec_diff_s(&t0, &t1);

  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    return false;
  }

  return true;
}

// ── Play one sim turn ──────────────────────────────────────────────────────
//
// Generates moves, runs the outer sim for budget_s seconds, and plays the
// sim-best move. Three inference modes:
//   - inference_results == NULL:  plain sim, uniform rack sampling.
//   - results_precomputed true:   inference_results already holds a (simmed)
//                                 leave distribution; simulate() skips its
//                                 internal infer().
//   - static_infer_args non-NULL: simulate() runs static inference itself
//                                 into inference_results (the mainline
//                                 sim-with-inference path).
// If a sim WITH inference fails, it is retried once without inference so an
// inference edge case doesn't degrade the arm to equity-best play.

static const Move *
play_sim_turn(Game *game, MoveList *move_list, SimResults *sim_results,
              SimCtx **sim_ctx, WinPct *win_pcts, ThreadControl *tc,
              int num_plies, double budget_s,
              InferenceResults *inference_results, bool results_precomputed,
              const InferenceArgs *static_infer_args, ErrorStack *error_stack) {
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

  const int num_moves = move_list_get_count(move_list);
  if (num_moves <= 1) {
    const Move *move = move_list_get_move(move_list, 0);
    play_move(move, game, NULL);
    return move;
  }

  // A zeroed InferenceArgs stands in when the distribution is precomputed:
  // sim_args_fill dereferences the pointer but simulate() never runs infer().
  InferenceArgs dummy_infer_args;
  memset(&dummy_infer_args, 0, sizeof(dummy_infer_args));

  bool sim_ok = false;
  for (int attempt = 0; attempt < 2 && !sim_ok; attempt++) {
    const bool with_inference = (attempt == 0) && (inference_results != NULL);
    // Non-inference attempts still pass the (ignored) dummy args so
    // sim_args_fill never sees a null pointer.
    const InferenceArgs *infer_args = &dummy_infer_args;
    if (with_inference && static_infer_args != NULL) {
      infer_args = static_infer_args;
    }

    SimArgs sim_args;
    // The budget is enforced with BAI's own time limit (which starts after
    // any internal inference), NOT an external interrupt timer: an interrupt
    // that fires while simulate() is still inside infer() would make it
    // return before resetting sim_results, and the caller would then read a
    // stale best move from the PREVIOUS turn (whose tiles may no longer be
    // on the rack).
    sim_args_fill(
        num_plies, move_list, /*num_plays=*/NUM_PLAYS,
        /*known_opp_rack=*/NULL, win_pcts,
        with_inference ? inference_results : NULL, tc, game, with_inference,
        /*use_heat_map=*/false, NUM_THREADS, /*print_interval=*/0, NUM_PLAYS,
        num_plies, /*seed=*/0, UINT64_MAX,
        /*min_play_iterations=*/50, /*scond=*/101.0, BAI_THRESHOLD_NONE,
        /*time_limit_seconds=*/budget_s, BAI_SAMPLING_RULE_TOP_TWO_IDS,
        /*cutoff=*/-1.0, infer_args, &sim_args);
    if (with_inference && results_precomputed) {
      sim_args.inference_results_precomputed = true;
    }

    thread_control_set_status(tc, THREAD_CONTROL_STATUS_STARTED);

    simulate(&sim_args, sim_ctx, sim_results, error_stack);

    if (error_stack_is_empty(error_stack)) {
      sim_ok = true;
    } else {
      error_stack_print_and_reset(error_stack);
      if (with_inference) {
        printf("    *** SIM WITH INFERENCE FAILED, retrying plain ***\n");
      }
    }
  }

  if (!sim_ok) {
    printf("    *** SIM FAILED, falling back to equity-best ***\n");
    const Move *move = move_list_get_move(move_list, 0);
    play_move(move, game, NULL);
    return move;
  }

  const BAIResult *bai_result = sim_results_get_bai_result(sim_results);
  int best_arm = bai_result_get_best_arm(bai_result);
  if (best_arm < 0 || best_arm >= num_moves) {
    best_arm = 0;
  }

  const Move *best_move =
      simmed_play_get_move(sim_results_get_simmed_play(sim_results, best_arm));
  play_move(best_move, game, NULL);
  return best_move;
}

// ── Play a full game ───────────────────────────────────────────────────────
// Returns: +1 if p0 wins, -1 if p1 wins, 0 if tie.

static int play_game(Game *game, MoveList *move_list, SimResults *sim_results,
                     SimCtx **sim_ctx, WinPct *win_pcts, ThreadControl *tc,
                     InferenceResults *inference_results, player_type_t p0_type,
                     player_type_t p1_type, uint64_t seed, int *p0_score_out,
                     int *p1_score_out, int *turns_out, double *elapsed_out) {
  game_reset(game);
  game_seed(game, seed);
  draw_starting_racks(game);

  ErrorStack *error_stack = error_stack_create();
  StringBuilder *sb = string_builder_create();

  struct timespec game_start, game_end;
  clock_gettime(CLOCK_MONOTONIC, &game_start);

  // Track the previous player's move for inference.
  game_event_t prev_move_type = GAME_EVENT_TILE_PLACEMENT_MOVE;
  int prev_player_index = -1;
  Move saved_prev_move;
  memset(&saved_prev_move, 0, sizeof(saved_prev_move));

  // Copy of the game state just before the previous player's move.
  Game *game_before_prev_move = game_duplicate(game);

  int turn = 0;
  while (!game_over(game)) {
    const int player_idx = game_get_player_on_turn_index(game);
    const player_type_t player_type = (player_idx == 0) ? p0_type : p1_type;
    const char *label = player_label(player_type);
    const Bag *bag = game_get_bag(game);
    const int bag_tiles = bag_get_letters(bag);
    int unseen = tiles_unseen(game);

    int effective_plies = NUM_PLIES;
    if (!bag_is_empty(bag) && unseen < LATE_GAME_TILE_THRESHOLD) {
      effective_plies = LATE_GAME_PLIES;
    }

    struct timespec turn_start, turn_end;
    clock_gettime(CLOCK_MONOTONIC, &turn_start);

    const Move *move = NULL;
    double simmed_infer_elapsed = 0.0;
    bool did_simmed_infer = false;
    bool simmed_infer_ok = false;

    // Inference (either kind) requires an inferable previous move and a
    // non-endgame bag.
    const bool can_infer = prev_player_index >= 0 &&
                           (prev_move_type == GAME_EVENT_TILE_PLACEMENT_MOVE ||
                            prev_move_type == GAME_EVENT_EXCHANGE) &&
                           bag_tiles >= RACK_SIZE;

    // Simmed inference runs BEFORE the outer sim and its elapsed time is
    // deducted from the sim budget; static inference runs INSIDE simulate()
    // (the mainline path) so it spends from the same budget automatically.
    InferenceResults *turn_inference_results = NULL;
    bool results_precomputed = false;
    PrevMoveInferSetup static_setup;
    const InferenceArgs *static_infer_args = NULL;

    if (player_type == PLAYER_SIMMEDINF && can_infer) {
      did_simmed_infer = true;
      simmed_infer_ok = run_simmed_inference(
          game_before_prev_move, win_pcts, tc, &saved_prev_move,
          prev_player_index, inference_results, error_stack,
          &simmed_infer_elapsed);
      if (simmed_infer_ok) {
        turn_inference_results = inference_results;
        results_precomputed = true;
      } else {
        printf("    *** SIMMED INFERENCE FAILED after %.2fs ***\n",
               simmed_infer_elapsed);
      }
    } else if (player_type == PLAYER_STATICINF && can_infer) {
      fill_prev_move_infer_args(&static_setup, game_before_prev_move,
                                &saved_prev_move, prev_player_index,
                                int_to_equity(STATIC_INFER_EQ_MARGIN), tc);
      static_infer_args = &static_setup.args;
      turn_inference_results = inference_results;
    }

    double sim_budget = SIM_BUDGET_S - simmed_infer_elapsed;
    if (sim_budget < MIN_SIM_BUDGET_S) {
      sim_budget = MIN_SIM_BUDGET_S;
    }

    // Save game state BEFORE play_sim_turn modifies it.
    Game *game_snapshot = game_duplicate(game);

    move = play_sim_turn(game, move_list, sim_results, sim_ctx, win_pcts, tc,
                         effective_plies, sim_budget, turn_inference_results,
                         results_precomputed, static_infer_args, error_stack);

    game_copy(game_before_prev_move, game_snapshot);
    game_destroy(game_snapshot);

    clock_gettime(CLOCK_MONOTONIC, &turn_end);
    double turn_elapsed = timespec_diff_s(&turn_start, &turn_end);
    turn++;

    string_builder_clear(sb);
    string_builder_add_move(sb, game_get_board(game), move, game_get_ld(game),
                            true);

    uint64_t iters = sim_results_get_iteration_count(sim_results);
    uint64_t num_infer_leaves = sim_results_get_num_infer_leaves(sim_results);
    if (static_infer_args != NULL) {
      printf("    Turn %2d (%-7s p%d): %s  [%.1fs, %" PRIu64
             " iters, statinf %" PRIu64 " leaves]\n",
             turn, label, player_idx, string_builder_peek(sb), turn_elapsed,
             iters, num_infer_leaves);
    } else if (did_simmed_infer) {
      printf("    Turn %2d (%-7s p%d): %s  [%.1fs, %" PRIu64
             " iters, siminf=%.2fs %" PRIu64 " racks%s]\n",
             turn, label, player_idx, string_builder_peek(sb), turn_elapsed,
             iters, simmed_infer_elapsed, num_infer_leaves,
             simmed_infer_ok ? "" : " FAILED");
    } else {
      printf("    Turn %2d (%-7s p%d): %s  [%.1fs, %" PRIu64 " iters]\n", turn,
             label, player_idx, string_builder_peek(sb), turn_elapsed, iters);
    }

    if (!error_stack_is_empty(error_stack)) {
      printf("  ERROR on turn %d: ", turn);
      error_stack_print_and_reset(error_stack);
    }

    prev_move_type = move_get_type(move);
    prev_player_index = player_idx;
    saved_prev_move = *move;
  }

  clock_gettime(CLOCK_MONOTONIC, &game_end);
  *elapsed_out = timespec_diff_s(&game_start, &game_end);
  *turns_out = turn;

  *p0_score_out = equity_to_int(player_get_score(game_get_player(game, 0)));
  *p1_score_out = equity_to_int(player_get_score(game_get_player(game, 1)));

  game_destroy(game_before_prev_move);
  string_builder_destroy(sb);
  error_stack_destroy(error_stack);

  if (*p0_score_out > *p1_score_out)
    return 1;
  if (*p1_score_out > *p0_score_out)
    return -1;
  return 0;
}

// ── Print running summary ──────────────────────────────────────────────────

static void print_pairing_line(const MatchResults *results, player_type_t a,
                               player_type_t b) {
  const int games = results->games[a][b];
  if (games == 0) {
    return;
  }
  printf("  %-7s vs %-7s: %3d - %3d (%d ties) in %3d games | %s spread "
         "%+ld (%+.1f/g)\n",
         player_label(a), player_label(b), results->wins[a][b],
         results->wins[b][a], results->ties[a][b], games, player_label(a),
         results->spread[a][b], (double)results->spread[a][b] / games);
}

static void print_summary(int seeds_done, const MatchResults *results) {
  printf("\n==== Results after %d seed(s) (%d games) ====\n", seeds_done,
         seeds_done * 6);
  print_pairing_line(results, PLAYER_SIMMEDINF, PLAYER_STATICINF);
  print_pairing_line(results, PLAYER_SIMMEDINF, PLAYER_NOINF);
  print_pairing_line(results, PLAYER_STATICINF, PLAYER_NOINF);
  // Overall per-player records across both pairings.
  for (int player = 0; player < NUM_PLAYER_TYPES; player++) {
    int wins = 0;
    int games = 0;
    for (int opp = 0; opp < NUM_PLAYER_TYPES; opp++) {
      wins += results->wins[player][opp];
      games += results->games[player][opp];
    }
    printf("  %-7s overall: %3d wins / %3d games (%.1f%%)\n",
           player_label((player_type_t)player), wins, games,
           games > 0 ? 100.0 * wins / games : 0.0);
  }
  printf("\n");
}

// Records one game's outcome into the crosstable. outcome is +1/-1/0 from
// p0's perspective.
static void record_game(MatchResults *results, player_type_t p0,
                        player_type_t p1, int outcome, int p0_score,
                        int p1_score) {
  // Normalize so the pair key is (lo, hi) with lo < hi; spread/ties/games are
  // stored at [hi][lo]... store spread and ties keyed by (a, b) where a is
  // the pairing's first-listed player: use (min, max) consistently.
  const player_type_t a = (p0 < p1) ? p0 : p1;
  const player_type_t b = (p0 < p1) ? p1 : p0;
  const int a_score = (a == p0) ? p0_score : p1_score;
  const int b_score = (a == p0) ? p1_score : p0_score;
  const int a_outcome = (a == p0) ? outcome : -outcome;
  results->games[a][b]++;
  results->games[b][a]++;
  results->spread[a][b] += a_score - b_score;
  results->spread[b][a] += b_score - a_score;
  if (a_outcome > 0) {
    results->wins[a][b]++;
  } else if (a_outcome < 0) {
    results->wins[b][a]++;
  } else {
    results->ties[a][b]++;
    results->ties[b][a]++;
  }
}

// ── Entry point ────────────────────────────────────────────────────────────

void test_simmedinf_benchmark(void) {
  setbuf(stdout, NULL);
  printf("\n");
  printf("========================================================\n");
  printf("  Inference round robin: NOINF / STATINF / SIMINF\n");
  printf("  %d seeds x 3 pairings x 2 seats = %d games\n", NUM_SEEDS,
         NUM_SEEDS * 6);
  printf("  %d threads, %d candidates, %d-ply outer sims\n", NUM_THREADS,
         NUM_PLAYS, NUM_PLIES);
  printf("  Turn budget: %.0fs (SIMINF: up to %.0fs simmed-infer deducted; "
         "STATINF margin %d)\n",
         SIM_BUDGET_S, SIMMED_INFER_BUDGET_S, STATIC_INFER_EQ_MARGIN);
  printf("  Inner sim: %d-ply, %d arms, probe=%d, full=%d, margin=%.1f\n",
         INNER_SIM_PLIES, INNER_NUM_PLAYS, INNER_PROBE_ITERS, INNER_FULL_ITERS,
         INNER_SIM_EQUITY_MARGIN);
  printf("  Late game: %d-ply when unseen < %d tiles\n", LATE_GAME_PLIES,
         LATE_GAME_TILE_THRESHOLD);
  printf("  Log: %s\n", LOG_FILENAME);
  printf("========================================================\n\n");

  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1 -threads 10");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);

  ErrorStack *wp_err = error_stack_create();
  WinPct *win_pcts =
      win_pct_create(config_get_data_paths(config), DEFAULT_WIN_PCT, wp_err);
  if (!error_stack_is_empty(wp_err)) {
    error_stack_print_and_reset(wp_err);
    log_fatal("failed to load win_pcts");
  }
  error_stack_destroy(wp_err);

  Game *game = config_get_game(config);
  MoveList *move_list = move_list_create(NUM_PLAYS);
  SimResults *sim_results = sim_results_create(0.0);
  SimCtx *sim_ctx = NULL;
  ThreadControl *tc = config_get_thread_control(config);
  InferenceResults *inference_results = inference_results_create(NULL);

  MatchResults results;
  memset(&results, 0, sizeof(results));

  static const player_type_t pairings[3][2] = {
      {PLAYER_SIMMEDINF, PLAYER_STATICINF},
      {PLAYER_SIMMEDINF, PLAYER_NOINF},
      {PLAYER_STATICINF, PLAYER_NOINF},
  };

  FILE *log_file = fopen(LOG_FILENAME, "w");
  if (log_file) {
    fprintf(log_file, "seed,pairing,seat_swap,p0_type,p1_type,p0_score,"
                      "p1_score,turns,elapsed_s,winner\n");
  }

  int game_number = 0;
  for (int seed_idx = 0; seed_idx < NUM_SEEDS; seed_idx++) {
    const uint64_t seed = (uint64_t)(seed_idx + 1);

    for (int pairing_idx = 0; pairing_idx < 3; pairing_idx++) {
      // Game pair: swap seats to cancel first-mover and draw advantage.
      for (int swap = 0; swap < 2; swap++) {
        const player_type_t p0 = pairings[pairing_idx][swap == 0 ? 0 : 1];
        const player_type_t p1 = pairings[pairing_idx][swap == 0 ? 1 : 0];

        game_number++;
        printf("  Game %3d (seed=%3" PRIu64 ", %s vs %s):\n", game_number, seed,
               player_label(p0), player_label(p1));

        int p0_score = 0;
        int p1_score = 0;
        int turns = 0;
        double elapsed = 0.0;

        const int outcome =
            play_game(game, move_list, sim_results, &sim_ctx, win_pcts, tc,
                      inference_results, p0, p1, seed, &p0_score, &p1_score,
                      &turns, &elapsed);

        record_game(&results, p0, p1, outcome, p0_score, p1_score);

        printf("    Final: %s %d - %s %d (%d turns, %.1fs)\n", player_label(p0),
               p0_score, player_label(p1), p1_score, turns, elapsed);

        if (log_file) {
          const char *winner_str = (outcome == 0)   ? "tie"
                                   : (outcome == 1) ? player_label(p0)
                                                    : player_label(p1);
          fprintf(log_file, "%" PRIu64 ",%d,%d,%s,%s,%d,%d,%d,%.2f,%s\n", seed,
                  pairing_idx, swap, player_label(p0), player_label(p1),
                  p0_score, p1_score, turns, elapsed, winner_str);
          fflush(log_file);
        }
      }
    }

    print_summary(seed_idx + 1, &results);
  }

  if (log_file) {
    fclose(log_file);
  }

  sim_ctx_destroy(sim_ctx);
  sim_results_destroy(sim_results);
  move_list_destroy(move_list);
  inference_results_destroy(inference_results);
  win_pct_destroy(win_pcts);
  config_destroy(config);
}
