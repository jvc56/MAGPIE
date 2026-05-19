// Simmed-inference benchmark: SIM4 vs SIMMEDINF4
//
// Usage: ./bin/magpie_test simmedinf
//
// Two player types compete in game pairs:
//   SIM4       — 4-ply simulation, uniform rack sampling (no inference).
//   SIMMEDINF4 — 4-ply simulation + simmed inference.
//                Before each sim turn, runs simmed_infer() on the opponent's
//                previous move to build a weighted leave distribution.
//                The outer sim then uses this precomputed distribution instead
//                of re-running static inference.
//
// Time budget per turn (approximate):
//   SIM4:       SIM_BUDGET_S seconds of outer sim.
//   SIMMEDINF4: SIMMED_INFER_BUDGET_S seconds for simmed inference,
//               then remaining time for outer sim.
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
#define SIM_BUDGET_S 4.0

// Simmed-inference time budget (seconds).  Must leave enough time for outer
// sim.  The outer sim receives at least MIN_SIM_BUDGET_S seconds.
#define SIMMED_INFER_BUDGET_S 3.0
#define MIN_SIM_BUDGET_S 1.0

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
  PLAYER_SIM4,       // 4-ply sim, uniform rack sampling
  PLAYER_SIMMEDINF4, // 4-ply sim + simmed inference (precomputed)
  NUM_PLAYER_TYPES,
} player_type_t;

static const char *player_label(player_type_t type) {
  switch (type) {
  case PLAYER_SIM4:
    return "SIM4";
  case PLAYER_SIMMEDINF4:
    return "SIMINF4";
  default:
    return "?????";
  }
}

// ── Per-game results ───────────────────────────────────────────────────────

typedef struct {
  int a_wins;
  int b_wins;
  int ties;
  int a_total_score;
  int b_total_score;
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
         rack_get_total_letters(player_get_rack(game_get_player(
             game, 1 - game_get_player_on_turn_index(game))));
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

// ── Run simmed inference on the opponent's previous move ──────────────────
//
// Populates inference_results with a weighted leave distribution based on
// inner sims. Returns the elapsed time and whether it completed (vs interrupt).

static bool run_simmed_inference(const Game *game_before_prev, WinPct *win_pcts,
                                 ThreadControl *tc, const Move *prev_move,
                                 int prev_player_index,
                                 InferenceResults *inference_results,
                                 ErrorStack *error_stack,
                                 double *elapsed_out) {
  const LetterDistribution *ld = game_get_ld(game_before_prev);
  const int ld_size = ld_get_size(ld);

  const game_event_t move_type = move_get_type(prev_move);

  Rack target_played_tiles;
  Rack target_known_rack;
  Rack nontarget_known_rack;
  rack_set_dist_size_and_reset(&target_known_rack, ld_size);
  rack_set_dist_size_and_reset(&nontarget_known_rack, ld_size);

  int target_num_exch = 0;
  if (move_type == GAME_EVENT_EXCHANGE) {
    target_num_exch = move_get_tiles_played(prev_move);
    rack_set_dist_size_and_reset(&target_played_tiles, ld_size);
  } else {
    extract_played_tiles(prev_move, ld_size, &target_played_tiles);
  }

  // Our rack (nontarget) is known to us
  const int nontarget_index = 1 - prev_player_index;
  rack_copy(&nontarget_known_rack,
            player_get_rack(
                game_get_player(game_before_prev, nontarget_index)));

  InferenceArgs base_args;
  infer_args_fill(&base_args, NUM_PLAYS, int_to_equity(0), NULL,
                  game_before_prev, NUM_THREADS, 0, tc,
                  /*use_game_history=*/false,
                  /*use_inference_cutoff_optimization=*/true,
                  prev_player_index, move_get_score(prev_move),
                  target_num_exch, &target_played_tiles, &target_known_rack,
                  &nontarget_known_rack);

  SimmedInferenceArgs si_args = {
      .base = &base_args,
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

  TimerArgs ta = {
      .tc = tc, .seconds = SIMMED_INFER_BUDGET_S, .done = false};
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

// ── Play one simmed turn ───────────────────────────────────────────────────
//
// Generates moves, runs the outer sim for budget_s seconds, and plays the
// sim-best move. If precomputed_inference_results is non-NULL, uses those
// results (skipping internal infer()) to bias rack sampling.

static const Move *
play_sim_turn(Game *game, MoveList *move_list, SimResults *sim_results,
              SimCtx **sim_ctx, WinPct *win_pcts, ThreadControl *tc,
              int num_plies, double budget_s,
              InferenceResults *precomputed_inference_results,
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
      .tiles_played_bv = NULL,
  };
  generate_moves(&gen_args);

  const int num_moves = move_list_get_count(move_list);
  if (num_moves <= 1) {
    const Move *move = move_list_get_move(move_list, 0);
    play_move(move, game, NULL);
    return move;
  }

  bool use_inference = (precomputed_inference_results != NULL);

  // When precomputed_inference_results is provided, we pass use_inference=true
  // but set inference_results_precomputed=true so simulate() skips infer().
  // We need a valid InferenceArgs when use_inference=true (sim_args_fill
  // dereferences it), even though simulate() won't call infer().
  InferenceArgs dummy_infer_args;
  memset(&dummy_infer_args, 0, sizeof(dummy_infer_args));

  SimArgs sim_args;
  sim_args_fill(num_plies, move_list, /*known_opp_rack=*/NULL, win_pcts,
                precomputed_inference_results, tc, game, use_inference,
                /*use_heat_map=*/false, NUM_THREADS, /*print_interval=*/0,
                NUM_PLAYS, num_plies, /*seed=*/0, UINT64_MAX,
                /*min_play_iterations=*/50, /*scond=*/101.0,
                BAI_THRESHOLD_NONE, /*time_limit_seconds=*/9999,
                BAI_SAMPLING_RULE_TOP_TWO_IDS, /*cutoff=*/-1.0,
                use_inference ? &dummy_infer_args : NULL, &sim_args);

  if (use_inference) {
    sim_args.inference_results_precomputed = true;
  }

  thread_control_set_status(tc, THREAD_CONTROL_STATUS_STARTED);

  TimerArgs ta = {.tc = tc, .seconds = budget_s, .done = false};
  pthread_t timer_tid;
  pthread_create(&timer_tid, NULL, timer_thread_func, &ta);

  simulate(&sim_args, sim_ctx, sim_results, error_stack);

  ta.done = true;
  pthread_join(timer_tid, NULL);

  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    printf("    *** SIM FAILED, falling back to equity-best ***\n");
    const Move *move = move_list_get_move(move_list, 0);
    play_move(move, game, NULL);
    return move;
  }

  BAIResult *bai_result = sim_results_get_bai_result(sim_results);
  int best_arm = bai_result_get_best_arm(bai_result);
  if (best_arm < 0) {
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
                     InferenceResults *inference_results,
                     player_type_t p0_type, player_type_t p1_type,
                     uint64_t seed, int *p0_score_out, int *p1_score_out,
                     int *turns_out, double *elapsed_out) {
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
    const player_type_t player_type =
        (player_idx == 0) ? p0_type : p1_type;
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

    // Simmed inference: run before choosing our move, based on opponent's
    // previous move.
    InferenceResults *precomputed = NULL;

    if (player_type == PLAYER_SIMMEDINF4 && prev_player_index >= 0 &&
        (prev_move_type == GAME_EVENT_TILE_PLACEMENT_MOVE ||
         prev_move_type == GAME_EVENT_EXCHANGE) &&
        bag_tiles >= RACK_SIZE) {
      did_simmed_infer = true;
      simmed_infer_ok = run_simmed_inference(
          game_before_prev_move, win_pcts, tc, &saved_prev_move,
          prev_player_index, inference_results, error_stack,
          &simmed_infer_elapsed);
      if (simmed_infer_ok) {
        precomputed = inference_results;
      } else {
        printf("    *** SIMMED INFERENCE FAILED after %.2fs ***\n",
               simmed_infer_elapsed);
      }
    }

    double sim_budget = SIM_BUDGET_S - simmed_infer_elapsed;
    if (sim_budget < MIN_SIM_BUDGET_S) {
      sim_budget = MIN_SIM_BUDGET_S;
    }

    // Save game state BEFORE play_sim_turn modifies it.
    Game *game_snapshot = game_duplicate(game);

    move = play_sim_turn(game, move_list, sim_results, sim_ctx, win_pcts, tc,
                         effective_plies, sim_budget, precomputed,
                         error_stack);

    game_copy(game_before_prev_move, game_snapshot);
    game_destroy(game_snapshot);

    clock_gettime(CLOCK_MONOTONIC, &turn_end);
    double turn_elapsed = timespec_diff_s(&turn_start, &turn_end);
    turn++;

    string_builder_clear(sb);
    string_builder_add_move(sb, game_get_board(game), move, game_get_ld(game),
                            true);

    uint64_t iters = sim_results_get_iteration_count(sim_results);
    uint64_t num_infer_leaves =
        sim_results_get_num_infer_leaves(sim_results);
    if (did_simmed_infer) {
      printf("    Turn %2d (%-7s p%d): %s  [%.1fs, %" PRIu64
             " iters, siminf=%.2fs %" PRIu64 " racks%s]\n",
             turn, label, player_idx, string_builder_peek(sb), turn_elapsed,
             iters, simmed_infer_elapsed, num_infer_leaves,
             simmed_infer_ok ? "" : " FAILED");
    } else {
      printf("    Turn %2d (%-7s p%d): %s  [%.1fs, %" PRIu64 " iters]\n",
             turn, label, player_idx, string_builder_peek(sb), turn_elapsed,
             iters);
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

static void print_summary(int seeds_done, const MatchResults *results) {
  printf("\n==== Results after %d seed(s) (%d games) ====\n", seeds_done,
         seeds_done * 2);
  int total = results->a_wins + results->b_wins + results->ties;
  printf("  %-7s: %3d wins / %3d games (%.1f%%), avg score %.1f\n",
         player_label(PLAYER_SIM4), results->a_wins, total,
         total > 0 ? 100.0 * results->a_wins / total : 0.0,
         total > 0 ? (double)results->a_total_score / total : 0.0);
  printf("  %-7s: %3d wins / %3d games (%.1f%%), avg score %.1f\n",
         player_label(PLAYER_SIMMEDINF4), results->b_wins, total,
         total > 0 ? 100.0 * results->b_wins / total : 0.0,
         total > 0 ? (double)results->b_total_score / total : 0.0);
  printf("  Ties: %d\n", results->ties);
  printf("  Spread: %+d\n\n",
         results->b_total_score - results->a_total_score);
}

// ── Entry point ────────────────────────────────────────────────────────────

void test_simmedinf_benchmark(void) {
  setbuf(stdout, NULL);
  printf("\n");
  printf("========================================================\n");
  printf("  Simmed-Inference Benchmark: SIM4 vs SIMMEDINF4\n");
  printf("  %d seeds × 2 (game pairs) = %d games\n", NUM_SEEDS, NUM_SEEDS * 2);
  printf("  %d threads, %d candidates, %d-ply outer sims\n", NUM_THREADS,
         NUM_PLAYS, NUM_PLIES);
  printf("  SIM4:      %.0fs outer sim budget\n", SIM_BUDGET_S);
  printf("  SIMMEDINF4: %.0fs simmed-infer + remaining for outer sim\n",
         SIMMED_INFER_BUDGET_S);
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
  WinPct *win_pcts = win_pct_create(config_get_data_paths(config),
                                    DEFAULT_WIN_PCT, wp_err);
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

  MatchResults results = {0};

  FILE *log_file = fopen(LOG_FILENAME, "w");
  if (log_file) {
    fprintf(log_file,
            "seed,game_pair,p0_type,p1_type,p0_score,p1_score,turns,"
            "elapsed_s,winner\n");
  }

  for (int seed_idx = 0; seed_idx < NUM_SEEDS; seed_idx++) {
    const uint64_t seed = (uint64_t)(seed_idx + 1);

    // Game pair: swap player positions to cancel first-mover advantage
    for (int swap = 0; swap < 2; swap++) {
      player_type_t p0 = (swap == 0) ? PLAYER_SIM4 : PLAYER_SIMMEDINF4;
      player_type_t p1 = (swap == 0) ? PLAYER_SIMMEDINF4 : PLAYER_SIM4;

      printf("  Game %3d (seed=%3" PRIu64 ", %s vs %s):\n",
             seed_idx * 2 + swap + 1, seed, player_label(p0),
             player_label(p1));

      int p0_score = 0, p1_score = 0, turns = 0;
      double elapsed = 0.0;

      int outcome = play_game(game, move_list, sim_results, &sim_ctx, win_pcts,
                              tc, inference_results, p0, p1, seed, &p0_score,
                              &p1_score, &turns, &elapsed);

      // Attribute win to the right player type
      player_type_t winner_type =
          (outcome == 0)   ? NUM_PLAYER_TYPES  // tie
          : (outcome == 1) ? p0
                           : p1;

      if (winner_type == PLAYER_SIM4) {
        results.a_wins++;
      } else if (winner_type == PLAYER_SIMMEDINF4) {
        results.b_wins++;
      } else {
        results.ties++;
      }

      // Accumulate scores per player type
      int sim4_score =
          (p0 == PLAYER_SIM4) ? p0_score : p1_score;
      int siminf_score =
          (p0 == PLAYER_SIMMEDINF4) ? p0_score : p1_score;
      results.a_total_score += sim4_score;
      results.b_total_score += siminf_score;

      printf("    Final: %s %d - %s %d (%d turns, %.1fs)\n",
             player_label(p0), p0_score, player_label(p1), p1_score, turns,
             elapsed);

      if (log_file) {
        const char *winner_str =
            (outcome == 0) ? "tie"
            : (outcome == 1) ? player_label(p0)
                             : player_label(p1);
        fprintf(log_file, "%" PRIu64 ",%d,%s,%s,%d,%d,%d,%.2f,%s\n", seed,
                swap, player_label(p0), player_label(p1), p0_score, p1_score,
                turns, elapsed, winner_str);
        fflush(log_file);
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
