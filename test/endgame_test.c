#include "../src/compat/cpthread.h"
#include "../src/compat/ctime.h"
#include "../src/def/cpthread_defs.h"
#include "../src/def/game_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/endgame_results.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/xoshiro.h"
#include "../src/impl/config.h"
#include "../src/impl/endgame.h"
#include "../src/impl/gameplay.h"
#include "../src/str/endgame_string.h"
#include "../src/str/game_string.h"
#include "../src/str/move_string.h"
#include "../src/str/rack_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_constants.h"
#include "test_util.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// Helper: append [Px wins by Y] / [Tie] based on spread delta
static void append_outcome(StringBuilder *sb, const Game *game,
                           int32_t spread_delta) {
  int on_turn = game_get_player_on_turn_index(game);
  int p1 = equity_to_int(player_get_score(game_get_player(game, 0)));
  int p2 = equity_to_int(player_get_score(game_get_player(game, 1)));
  int final_spread = (p1 - p2) + (on_turn == 0 ? spread_delta : -spread_delta);
  if (final_spread > 0) {
    string_builder_add_formatted_string(sb, " [P1 wins by %d]", final_spread);
  } else if (final_spread < 0) {
    string_builder_add_formatted_string(sb, " [P2 wins by %d]", -final_spread);
  } else {
    string_builder_add_string(sb, " [Tie]");
  }
}

// Format a PVLine into a string builder (moves separated by spaces, with
// end-of-game annotations and | between negamax/greedy moves)
static void format_pvline(StringBuilder *sb, const PVLine *pv_line,
                          const Game *game) {
  const LetterDistribution *ld = game_get_ld(game);
  Move move;
  Game *gc = game_duplicate(game);
  for (int i = 0; i < pv_line->num_moves; i++) {
    small_move_to_move(&move, &(pv_line->moves[i]), game_get_board(gc));
    string_builder_add_move(sb, game_get_board(gc), &move, ld, true);
    play_move(&move, gc, NULL);
    if (game_get_game_end_reason(gc) == GAME_END_REASON_STANDARD) {
      int opp_idx = game_get_player_on_turn_index(gc);
      const Rack *opp_rack = player_get_rack(game_get_player(gc, opp_idx));
      int adj = equity_to_int(calculate_end_rack_points(opp_rack, ld));
      string_builder_add_string(sb, " (");
      string_builder_add_rack(sb, opp_rack, ld, false);
      string_builder_add_formatted_string(sb, " +%d)", adj);
    } else if (game_get_game_end_reason(gc) ==
               GAME_END_REASON_CONSECUTIVE_ZEROS) {
      string_builder_add_string(sb, " (6 zeros)");
    }
    if (i < pv_line->num_moves - 1) {
      if (i + 1 == pv_line->negamax_depth && pv_line->negamax_depth > 0) {
        string_builder_add_string(sb, " |");
      }
      string_builder_add_string(sb, " ");
    }
  }
  game_destroy(gc);
}

// Per-ply callback to print PV and ranked root moves during iterative
// deepening
static void print_pv_callback(int depth, int32_t value, const PVLine *pv_line,
                              const Game *game, const PVLine *ranked_pvs,
                              int num_ranked_pvs, void *user_data) {
  const Timer *timer = (const Timer *)user_data;
  double elapsed = ctimer_elapsed_seconds(timer);

  // Print best PV line
  StringBuilder *sb = string_builder_create();
  string_builder_add_formatted_string(
      sb, "  depth %d: value=%d, time=%.3fs, pv=", depth, value, elapsed);
  format_pvline(sb, pv_line, game);
  append_outcome(sb, game, value);
  printf("%s\n", string_builder_peek(sb));
  string_builder_destroy(sb);

  (void)ranked_pvs;
  (void)num_ranked_pvs;
}

// Per-ply callback that also prints ranked root moves
static void print_pv_and_ranked_callback(int depth, int32_t value,
                                         const PVLine *pv_line,
                                         const Game *game,
                                         const PVLine *ranked_pvs,
                                         int num_ranked_pvs, void *user_data) {
  print_pv_callback(depth, value, pv_line, game, ranked_pvs, num_ranked_pvs,
                    user_data);
  for (int i = 0; i < num_ranked_pvs; i++) {
    StringBuilder *sb = string_builder_create();
    string_builder_add_formatted_string(sb, "    %2d. value=%d, pv=", i + 1,
                                        ranked_pvs[i].score);
    format_pvline(sb, &ranked_pvs[i], game);
    append_outcome(sb, game, ranked_pvs[i].score);
    printf("%s\n", string_builder_peek(sb));
    string_builder_destroy(sb);
  }
}

typedef struct {
  Config *config;
  int timeout;
} TimeoutThreadArgs;

static void *timeout_thread_function(void *arg) {
  const TimeoutThreadArgs *args = (TimeoutThreadArgs *)arg;
  char *endgame_string = endgame_results_get_string(
      config_get_endgame_results(args->config), config_get_game(args->config),
      config_get_game_history(args->config));
  free(endgame_string);
  for (int i = 0; i < args->timeout; i++) {
    ctime_nap(1);
    endgame_string = endgame_results_get_string(
        config_get_endgame_results(args->config), config_get_game(args->config),
        config_get_game_history(args->config));
    printf("%s\n", endgame_string);
    free(endgame_string);
  }
  thread_control_set_status(config_get_thread_control(args->config),
                            THREAD_CONTROL_STATUS_USER_INTERRUPT);
  return NULL;
}

void test_single_endgame(const char *config_settings, const char *cgp,
                         int initial_small_move_arena_size,
                         error_code_t expected_error_code,
                         const int expected_score, const bool is_pass,
                         int timeout) {
  // Load config
  Config *config = config_create_or_die(config_settings);
  load_and_exec_config_or_die(config, cgp);

  // Create solver
  EndgameCtx *endgame_ctx = NULL;

  // Create args
  Game *game = config_get_game(config);
  Timer timer;
  ctimer_start(&timer);
  EndgameArgs endgame_args = {0};
  endgame_args.thread_control = config_get_thread_control(config);
  endgame_args.game = game;
  endgame_args.plies = config_get_endgame_plies(config);
  endgame_args.tt_fraction_of_mem = config_get_tt_fraction_of_mem(config);
  endgame_args.initial_small_move_arena_size = initial_small_move_arena_size;
  endgame_args.num_threads = 6;
  endgame_args.use_heuristics = true;
  endgame_args.forced_pass_bypass = true;
  endgame_args.enable_pv_display = true;
  endgame_args.num_top_moves = 1;
  endgame_args.per_ply_callback = print_pv_callback;
  endgame_args.per_ply_callback_data = &timer;
  endgame_args.seed = 42;

  if (timeout > 0) {
    endgame_args.num_top_moves = 10;
  }

  // Create results
  EndgameResults *endgame_results = config_get_endgame_results(config);

  // Create error stack
  ErrorStack *error_stack = error_stack_create();

  // Print the game position and ply count
  StringBuilder *game_sb = string_builder_create();
  GameStringOptions *gso = game_string_options_create_default();
  string_builder_add_game(game, NULL, gso, NULL, game_sb);
  printf("\n%s\n", string_builder_peek(game_sb));
  string_builder_destroy(game_sb);
  game_string_options_destroy(gso);

  // Create timeout thread if timeout is nonzero
  cpthread_t timeout_thread_id;
  TimeoutThreadArgs timeout_args = {.config = config, .timeout = timeout};
  if (timeout > 0) {
    cpthread_create(&timeout_thread_id, timeout_thread_function, &timeout_args);
  }

  printf("Solving %d-ply endgame with %d threads...\n", endgame_args.plies,
         endgame_args.num_threads);
  endgame_solve(&endgame_ctx, &endgame_args, endgame_results, error_stack);

  // Join the timeout thread if it was created
  if (timeout > 0) {
    cpthread_join(timeout_thread_id);
  }

  const error_code_t actual_error_code = error_stack_top(error_stack);
  const bool has_expected_error = actual_error_code == expected_error_code;
  if (!has_expected_error) {
    printf("encountered expected error code in endgame test\nexpected: "
           "%d\nactual:   %d",
           expected_error_code, actual_error_code);
    error_stack_print_and_reset(error_stack);
    assert(0);
  }

  if (actual_error_code == ERROR_STATUS_SUCCESS && timeout == 0) {
    const PVLine *pv_line =
        endgame_results_get_pvline(endgame_results, ENDGAME_RESULT_BEST);
    assert(pv_line->score == expected_score);
    assert(small_move_is_pass(&pv_line->moves[0]) == is_pass);
    printf(
        "Endgame solved successfully with expected score %d and is_pass=%d\n",
        expected_score, is_pass);
  }

  char *endgame_string =
      endgame_results_get_string(endgame_results, game, NULL);
  printf("\nFinal Endgame Outputs:\n%s\n", endgame_string);
  free(endgame_string);

  endgame_ctx_destroy(endgame_ctx);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

void test_vs_joey(void) {
  /*
  This one is kind of tricky! You need to look at least 13 plies deep to
  find the right sequence, even though it is only 7 moves long:

  Best sequence has a spread difference (value) of +55
  Final spread after seq: +14
  Best sequence:
  1) J11 .R (2)
  2)  F2 DI. (19)
  3)  6N .I (11)
  4) (Pass) (0)
  5) 12I E. (4)
  6) (Pass) (0)
  7) H12 FLAM (49)
  */

  test_single_endgame(
      "set -s1 score -s2 score -threads 6 -eplies 13",
      "cgp "
      "AIDER2U7/b1E1E2N1Z5/AWN1T2M1ATT3/LI1COBLE2OW3/OP2U2E2AA3/NE2CUSTARDS1Q1/"
      "ER1OH5I2U1/S2K2FOB1ERGOT/5HEXYLS2I1/4JIN6N1/2GOOP2NAIVEsT/1DIRE10/"
      "2GAY10/15/15 AEFILMR/DIV 371/412 0 -lex NWL20;",
      DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE, ERROR_STATUS_SUCCESS, 55, false,
      0);
}

void test_pass_first(void) {
  // This endgame's first move must be a pass, otherwise Nigel can set up
  // an unblockable ZA. Exact value is -63 (verified at 25-ply with and
  // without heuristics). The optimal PV is 10 moves with 4 passes, so
  // 7-ply relies on greedy playout for the tail.
  test_single_endgame(
      "set -s1 score -s2 score -threads 6 -eplies 7",
      "cgp "
      "GATELEGs1POGOED/R4MOOLI3X1/AA10U2/YU4BREDRIN2/1TITULE3E1IN1/1E4N3c1BOK/"
      "1C2O4CHARD1/QI1FLAWN2E1OE1/IS2E1HIN1A1W2/1MOTIVATE1T1S2/1S2N5S4/"
      "3PERJURY5/15/15/15 FV/AADIZ 442/388 0 -lex CSW21",
      DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE, ERROR_STATUS_SUCCESS, -63, true,
      0);
}

// Drives endgame_add_worker from inside the running solve: the per-ply
// callback fires on the master worker after each completed depth, and injects
// one extra ABDADA worker per ply (up to max_attempts) — modeling a pool that
// lends idle cores mid-search. Each injected worker draws its own MoveGen cache
// slot from get_movegen() (per-pthread), so the injector hands out no slots.
typedef struct WorkerInjector {
  EndgameCtx **solver_ptr; // address of the caller's ctx (set by endgame_solve)
  int max_attempts;        // cap on injection attempts (one per ply)
  int attempts;            // attempts made so far
  int added;               // successful injections
} WorkerInjector;

static void inject_worker_per_ply(int depth, int32_t value,
                                  const PVLine *pv_line, const Game *game,
                                  const PVLine *ranked_pvs, int num_ranked_pvs,
                                  void *user_data) {
  (void)depth;
  (void)value;
  (void)pv_line;
  (void)game;
  (void)ranked_pvs;
  (void)num_ranked_pvs;
  WorkerInjector *injector = (WorkerInjector *)user_data;
  EndgameCtx *solver = *injector->solver_ptr;
  if (solver == NULL || injector->attempts >= injector->max_attempts) {
    return;
  }
  if (endgame_add_worker(solver)) {
    injector->added++;
  }
  injector->attempts++;
}

// Dynamic worker injection: a solve started with one thread must absorb extra
// ABDADA workers injected mid-search (as cores free up) and still return the
// exact single-threaded value. ABDADA is value-deterministic regardless of
// thread count or when workers join, so the result must equal the known -63.
// Exercised for both endgame_solve (master is a spawned thread) and
// endgame_solve_inline (master runs in the calling thread — the path PEG uses,
// where injected helpers cooperate while the caller's own core keeps solving).
// 6 plies is the shallowest depth that reaches the converged -63 (5 gives -60),
// and still injects one worker per IDS depth — the value-determinism property
// is what the test checks, not the search depth.
static void run_injection_case(bool use_inline) {
  Config *config =
      config_create_or_die("set -s1 score -s2 score -threads 1 -eplies 6");
  load_and_exec_config_or_die(
      config,
      "cgp "
      "GATELEGs1POGOED/R4MOOLI3X1/AA10U2/YU4BREDRIN2/1TITULE3E1IN1/1E4N3c1BOK/"
      "1C2O4CHARD1/QI1FLAWN2E1OE1/IS2E1HIN1A1W2/1MOTIVATE1T1S2/1S2N5S4/"
      "3PERJURY5/15/15/15 FV/AADIZ 442/388 0 -lex CSW21");

  Game *game = config_get_game(config);
  EndgameResults *results = config_get_endgame_results(config);

  EndgameCtx *ctx = NULL;
  // Model a pool lending up to 4 idle cores mid-search (one per ply).
  WorkerInjector injector = {
      .solver_ptr = &ctx, .max_attempts = 4, .attempts = 0, .added = 0};

  EndgameArgs args = {0};
  args.thread_control = config_get_thread_control(config);
  args.game = game;
  args.plies = config_get_endgame_plies(config);
  // Tiny TT (clamps to the 2^24 minimum) instead of the 0.25 default: this
  // small endgame needs almost no TT, and allocating + zeroing a multi-GB
  // table twice (once per case) dominated the test's runtime. The solved value
  // is exact regardless of TT size, so this only affects speed.
  args.tt_fraction_of_mem = 0.0001;
  args.initial_small_move_arena_size = DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE;
  args.num_threads = 1; // start single-threaded...
  args.max_workers = 5; // ...and allow growth to 1 + 4 injected
  args.use_heuristics = true;
  args.forced_pass_bypass = true;
  args.num_top_moves = 1;
  args.seed = 42;
  args.per_ply_callback = inject_worker_per_ply;
  args.per_ply_callback_data = &injector;

  ErrorStack *error_stack = error_stack_create();
  if (use_inline) {
    endgame_solve_inline(&ctx, &args, results);
  } else {
    endgame_solve(&ctx, &args, results, error_stack);
  }
  assert(error_stack_is_empty(error_stack));
  const int32_t value =
      endgame_results_get_pvline(results, ENDGAME_RESULT_BEST)->score;

  printf("dynamic-worker-injection (%s): value=%d workers_added=%d\n",
         use_inline ? "inline" : "spawned", value, injector.added);
  assert(value == -63);
  assert(injector.added > 0); // at least one helper joined mid-search

  error_stack_destroy(error_stack);
  endgame_ctx_destroy(ctx);
  config_destroy(config);
}

void test_endgame_dynamic_worker_injection(void) {
  run_injection_case(/*use_inline=*/false);
  run_injection_case(/*use_inline=*/true);
}

// first_win uses a narrow [-1,+1] window to resolve win/loss/draw cheaply (the
// path pessimistic PEG uses), and caps the depth-0 interrupt fallback to the
// top few root moves. It must agree in SIGN with the exact full-spread solve.
// This position is a converged 63-point loss for the player on turn at 6 plies:
// full-spread must report exactly -63, first_win a loss (strictly negative).
void test_endgame_first_win_sign(void) {
  const char *cgp =
      "cgp "
      "GATELEGs1POGOED/R4MOOLI3X1/AA10U2/YU4BREDRIN2/1TITULE3E1IN1/1E4N3c1BOK/"
      "1C2O4CHARD1/QI1FLAWN2E1OE1/IS2E1HIN1A1W2/1MOTIVATE1T1S2/1S2N5S4/"
      "3PERJURY5/15/15/15 FV/AADIZ 442/388 0 -lex CSW21";
  // {first_win, first_win_fallback_moves}: full-spread, then first-win with the
  // default fallback cap, then first-win skipping the fallback entirely (-1).
  // first-win must agree in SIGN with the exact solve regardless of the
  // (adjustable) fallback cap.
  const struct {
    bool first_win;
    int fallback;
  } cases[] = {{false, 0}, {true, 0}, {true, -1}};
  for (size_t ci = 0; ci < sizeof(cases) / sizeof(cases[0]); ci++) {
    Config *config =
        config_create_or_die("set -s1 score -s2 score -threads 1 -eplies 6");
    load_and_exec_config_or_die(config, cgp);
    Game *game = config_get_game(config);
    EndgameResults *results = config_get_endgame_results(config);

    EndgameCtx *ctx = NULL;
    EndgameArgs args = {0};
    args.thread_control = config_get_thread_control(config);
    args.game = game;
    args.plies = config_get_endgame_plies(config);
    args.tt_fraction_of_mem = 0.0001;
    args.initial_small_move_arena_size = DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE;
    args.num_threads = 1;
    args.max_workers = 1;
    args.use_heuristics = true;
    args.forced_pass_bypass = true;
    args.num_top_moves = 1;
    args.seed = 42;
    args.first_win = cases[ci].first_win;
    args.first_win_fallback_moves = cases[ci].fallback;

    endgame_solve_inline(&ctx, &args, results);
    const int32_t value =
        endgame_results_get_pvline(results, ENDGAME_RESULT_BEST)->score;
    printf("endgame first_win=%d fallback=%d: value=%d\n", args.first_win,
           args.first_win_fallback_moves, value);
    if (args.first_win) {
      assert(value < 0); // correct loss sign regardless of the fallback cap
    } else {
      assert(value == -63); // exact full-spread value
    }
    endgame_ctx_destroy(ctx);
    config_destroy(config);
  }
}

void test_nonempty_bag(void) {
  // The solver should return an error if the bag is not empty.
  test_single_endgame("set -s1 score -s2 score -threads 6 -eplies 4",
                      "cgp " EMPTY_CGP, DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
                      ERROR_STATUS_ENDGAME_BAG_NOT_EMPTY, 0, false, 0);
}

void test_single_pv_display(void) {
  // Solve a multi-PV endgame, then verify that shendgame <n> produces output
  // via string_builder_endgame_single_pv for each valid PV index .
  Config *config =
      config_create_or_die("set -s1 score -s2 score -eplies 4 -etopk 5");
  load_and_exec_config_or_die(
      config, "cgp "
              "9A1PIXY/9S1L3/2ToWNLETS1O3/9U1DA1R/3GERANIAL1U1I/9g2T1C/8WE2OBI/"
              "6EMU4ON/6AID3GO1/5HUN4ET1/4ZA1T4ME1/1Q1FAKEY3JOES/FIVE1E5IT1C/"
              "5SPORRAN2A/6ORE2N2D BGIV/DEHILOR 384/389 0 -lex NWL20");

  Game *game = config_get_game(config);
  EndgameResults *endgame_results = config_get_endgame_results(config);
  ErrorStack *error_stack = error_stack_create();
  EndgameCtx *endgame_ctx = NULL;

  EndgameArgs endgame_args = {0};
  endgame_args.thread_control = config_get_thread_control(config);
  endgame_args.game = game;
  endgame_args.plies = config_get_endgame_plies(config);
  endgame_args.tt_fraction_of_mem = config_get_tt_fraction_of_mem(config);
  endgame_args.initial_small_move_arena_size =
      DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE;
  endgame_args.num_threads = 6;
  endgame_args.use_heuristics = true;
  endgame_args.forced_pass_bypass = true;
  endgame_args.enable_pv_display = true;
  endgame_args.num_top_moves = 5;
  endgame_args.seed = 42;

  endgame_solve(&endgame_ctx, &endgame_args, endgame_results, error_stack);
  assert(error_stack_is_empty(error_stack));

  const int num_pvs = endgame_results_get_num_pvs(endgame_results);
  assert(num_pvs > 1);

  const Game *source_game = endgame_results_get_start_game(endgame_results);

  // Each valid 0-indexed PV should produce non-empty output.
  for (int pv_idx = 0; pv_idx < num_pvs; pv_idx++) {
    StringBuilder *sb = string_builder_create();
    string_builder_endgame_single_pv(sb, endgame_results, source_game, NULL,
                                     pv_idx);
    const char *output = string_builder_peek(sb);
    assert(output && output[0] != '\0');
    printf("single_pv display %d/%d:\n%s\n", pv_idx + 1, num_pvs, output);
    string_builder_destroy(sb);
  }

  endgame_ctx_destroy(endgame_ctx);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

void test_ctx_reuse(void) {
  // Solve the same position multiple times with the same EndgameCtx, varying
  // the thread count each solve to exercise worker-pool growth/reuse and
  // the thread_capacity / worker_ids allocation paths.
  const char *cgp =
      "cgp "
      "9A1PIXY/9S1L3/2ToWNLETS1O3/9U1DA1R/3GERANIAL1U1I/9g2T1C/8WE2OBI/"
      "6EMU4ON/6AID3GO1/5HUN4ET1/4ZA1T4ME1/1Q1FAKEY3JOES/FIVE1E5IT1C/"
      "5SPORRAN2A/6ORE2N2D BGIV/DEHILOR 384/389 0 -lex NWL20";

  Config *config = config_create_or_die("set -s1 score -s2 score -eplies 4");
  load_and_exec_config_or_die(config, cgp);

  Game *game = config_get_game(config);
  EndgameResults *endgame_results = config_get_endgame_results(config);
  ErrorStack *error_stack = error_stack_create();
  EndgameCtx *endgame_ctx = NULL;

  const int thread_counts[] = {1, 3, 6, 2, 1, 4};
  const int num_solves = 6;
  for (int solve_idx = 0; solve_idx < num_solves; solve_idx++) {
    EndgameArgs endgame_args = {0};
    endgame_args.thread_control = config_get_thread_control(config);
    endgame_args.game = game;
    endgame_args.plies = config_get_endgame_plies(config);
    endgame_args.tt_fraction_of_mem = config_get_tt_fraction_of_mem(config);
    endgame_args.initial_small_move_arena_size =
        DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE;
    endgame_args.num_threads = thread_counts[solve_idx];
    endgame_args.use_heuristics = true;
    endgame_args.forced_pass_bypass = true;
    endgame_args.enable_pv_display = true;
    endgame_args.num_top_moves = 1;
    endgame_args.seed = 42;

    printf("ctx reuse solve %d/%d: %d thread(s)\n", solve_idx + 1, num_solves,
           endgame_args.num_threads);
    endgame_solve(&endgame_ctx, &endgame_args, endgame_results, error_stack);
    assert(error_stack_is_empty(error_stack));

    const PVLine *pv_line =
        endgame_results_get_pvline(endgame_results, ENDGAME_RESULT_BEST);
    assert(pv_line->score == 11);
    assert(!small_move_is_pass(&pv_line->moves[0]));
  }

  endgame_ctx_destroy(endgame_ctx);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

void test_solve_standard(void) {
  // A standard out-in-two endgame.
  test_single_endgame(
      "set -s1 score -s2 score -threads 6 -eplies 4",
      "cgp "
      "9A1PIXY/9S1L3/2ToWNLETS1O3/9U1DA1R/3GERANIAL1U1I/9g2T1C/8WE2OBI/"
      "6EMU4ON/6AID3GO1/5HUN4ET1/4ZA1T4ME1/1Q1FAKEY3JOES/FIVE1E5IT1C/"
      "5SPORRAN2A/6ORE2N2D BGIV/DEHILOR 384/389 0 -lex NWL20",
      DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE, ERROR_STATUS_SUCCESS, 11, false,
      0);
}

void test_very_deep(void) {
  // This insane endgame requires 25 plies to solve. We end up winning by 1 pt.
  test_single_endgame(
      "set -s1 score -s2 score -threads 6 -eplies 25",
      "cgp "
      "14C/13QI/12FIE/10VEE1R/9KIT2G/8CIG1IDE/8UTA2AS/7ST1SYPh1/6JA5A1/"
      "5WOLD2BOBA/3PLOT1R1NU1EX/Y1VEIN1NOR1mOA1/UT1AT1N1L2FEH1/"
      "GUR2WIRER5/SNEEZED8 ADENOOO/AHIILMM 353/236 0 -lex CSW21;",
      DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE, ERROR_STATUS_SUCCESS, -116, true,
      0);
}

void test_eldar_v_stick(void) {
  test_single_endgame(
      "set -s1 score -s2 score -threads 6 -eplies 3",
      "cgp "
      "4EXODE6/1DOFF1KERATIN1U/1OHO8YEN/1POOJA1B3MEWS/5SQUINTY2A/4RHINO1e3V/"
      "2B4C2R3E/GOAT1D1E2ZIN1d/1URACILS2E4/1PIG1S4T4/2L2R4T4/2L2A1GENII3/"
      "2A2T1L7/5E1A7/5D1M7 AEEIRUW/V 410/409 0 -lex CSW21;",
      DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE, ERROR_STATUS_SUCCESS, 72, false,
      0);
}

void test_small_arena_realloc(void) {
  test_single_endgame(
      "set -s1 score -s2 score -threads 6 -eplies 4",
      "cgp "
      "9A1PIXY/9S1L3/2ToWNLETS1O3/9U1DA1R/3GERANIAL1U1I/9g2T1C/8WE2OBI/"
      "6EMU4ON/6AID3GO1/5HUN4ET1/4ZA1T4ME1/1Q1FAKEY3JOES/FIVE1E5IT1C/"
      "5SPORRAN2A/6ORE2N2D BGIV/DEHILOR 384/389 0 -lex NWL20",
      512, ERROR_STATUS_SUCCESS, 11, false, 0);
}

void test_endgame_interrupt(void) {
  test_single_endgame(
      "set -s1 score -s2 score -threads 1 -eplies 25 -etopk 10",
      "cgp "
      "4EXODE6/1DOFF1KERATIN1U/1OHO8YEN/1POOJA1B3MEWS/5SQUINTY2A/4RHINO1e3V/"
      "2B4C2R3E/GOAT1D1E2ZIN1d/1URACILS2E4/1PIG1S4T4/2L2R4T4/2L2A1GENII3/"
      "2A2T1L7/5E1A7/5D1M7 AEEIRUW/V 410/409 0 -lex CSW21;",
      DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE, ERROR_STATUS_SUCCESS, 0, true, 3);
}

void test_kue(void) {
  Config *config = config_create_or_die("set -s1 score -s2 score -eplies 14 "
                                        "-ttfraction 0.5");
  load_and_exec_config_or_die(
      config, "cgp "
              "6MOO1VIRLS/1EJECTA6A1/2I2AEON4R1/2BAH6X1N1/2SLID4GIFTS/"
              "4DONG1OR1R1i/7HOURLY1Z/FE4DINT1A2Y/RECLINE2I1N3/"
              "EW1ATAP2E1G3/M10U3/D3PATOOTIE3/15/15/15 "
              "?AEEKSU/BEIQUVW 276/321 0 -lex NWL23;");

  EndgameCtx *endgame_ctx = NULL;
  Game *game = config_get_game(config);
  Timer timer;
  ctimer_start(&timer);

  EndgameArgs endgame_args = {0};
  endgame_args.thread_control = config_get_thread_control(config);
  endgame_args.game = game;
  endgame_args.plies = 14;
  endgame_args.tt_fraction_of_mem = 0.5;
  endgame_args.initial_small_move_arena_size =
      DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE;
  endgame_args.num_threads = 8;
  endgame_args.use_heuristics = true;
  endgame_args.num_top_moves = 10;
  endgame_args.per_ply_callback = print_pv_and_ranked_callback;
  endgame_args.per_ply_callback_data = &timer;
  endgame_args.seed = 42;

  EndgameResults *endgame_results = config_get_endgame_results(config);
  ErrorStack *error_stack = error_stack_create();

  StringBuilder *game_sb = string_builder_create();
  GameStringOptions *gso = game_string_options_create_default();
  string_builder_add_game(game, NULL, gso, NULL, game_sb);
  printf("\n%s\n", string_builder_peek(game_sb));
  string_builder_destroy(game_sb);
  game_string_options_destroy(gso);

  printf("Solving %d-ply endgame with %d threads, top %d moves, "
         "ttfraction=%.1f...\n",
         endgame_args.plies, endgame_args.num_threads,
         endgame_args.num_top_moves, endgame_args.tt_fraction_of_mem);
  endgame_solve(&endgame_ctx, &endgame_args, endgame_results, error_stack);
  assert(error_stack_is_empty(error_stack));

  const PVLine *pv_line =
      endgame_results_get_pvline(endgame_results, ENDGAME_RESULT_BEST);
  assert(pv_line->score == 52);

  endgame_ctx_destroy(endgame_ctx);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

// QI-relevant 2-lexicon endgame test.
// TWL98 vs CSW24 - TWL98 doesn't have QI, CSW24 does.
// Position from TWL98 vs CSW24 game (seed 1023).
// Player 1 (TWL98) has IQV - has Q and I but can't play QI in TWL98.
// CSW24 player on turn.
#define TWO_LEXICON_CGP                                                        \
  "cgp "                                                                       \
  "DOBIE2ARCSINES/1FANWORT4OX1/7O3FROM/7K3L3/6VEEJAY3/11MOA1/"                 \
  "7PIgWEEDS/5DUI3N2H/5ETUI4TI/3GUP7AL/13NY/4TItHONIA1G1/"                     \
  "7E5L1/4RECRATE2E1/7D1ANGORA BELSTUZ/IQV 373/426 0"

void test_2lex_endgame(dual_lexicon_mode_t mode, int expected_score) {
  Config *config = config_create_or_die(
      "set -l1 TWL98 -l2 CSW24 -wmp false -s1 score -s2 score"
      " -threads 1 -eplies 4");
  load_and_exec_config_or_die(config, TWO_LEXICON_CGP);

  EndgameCtx *solver = NULL;
  Game *game = config_get_game(config);
  ErrorStack *error_stack = error_stack_create();
  EndgameResults *results = config_get_endgame_results(config);

  EndgameArgs args = {
      .thread_control = config_get_thread_control(config),
      .game = game,
      .plies = 3,
      .tt_fraction_of_mem = 0.05,
      .initial_small_move_arena_size = DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
      .num_threads = 1,
      .num_top_moves = 1,
      .use_heuristics = false,
      .per_ply_callback = NULL,
      .per_ply_callback_data = NULL,
      .dual_lexicon_mode = mode,
      .seed = 42,
  };

  endgame_solve(&solver, &args, results, error_stack);
  assert(error_stack_is_empty(error_stack));
  const PVLine *pv_line =
      endgame_results_get_pvline(results, ENDGAME_RESULT_BEST);
  assert(pv_line->score == expected_score);

  error_stack_destroy(error_stack);
  endgame_ctx_destroy(solver);
  config_destroy(config);
}

void test_2lex_ignorant(void) {
  test_2lex_endgame(DUAL_LEXICON_MODE_IGNORANT, 96);
}

void test_2lex_informed(void) {
  test_2lex_endgame(DUAL_LEXICON_MODE_INFORMED, 81);
}

// Pos 28 from the 2peg random suite (CSW24). After mover plays 4L (V)IA 19
// in scenario IS (mover drew I,S from bag): bag empty, opp has AIINOOU,
// mover has CDIOST?. The blank gives mover a DISCO(U)nT bingo at 1A through
// the existing U at row 1 col F, scoring 167 + 14 going-out bonus = +181
// value. Opp's only defence is 2B ONIO(N) 10 which creates an "IO" cross
// below 1B and BLOCKS DISCO(U)nT. Discovered while debugging the peg
// greedy d=0 evaluator: a tight -tlim that interrupts IDS in the middle
// of its depth-3 iteration was returning pass-pass at "status: finished"
// even though -plies 3 with adequate time gives the correct answer.
//
// The tests below verify both halves of the response at every search depth:
// (1) from mover's turn (after opp would pass), mover MUST find DISCO(U)nT
// (2) from opp's turn, opp MUST block and the PV ends in mover's bingo.

#define VIA_IS_OPP_TURN_CGP                                                    \
  "cgp 5U4OHMIC/5N3WREATH/5T4FAX2/5i3B1VIA1/5N3L1E3/5G2VELDT2/5E3S5/"          \
  "5DREKS1F3/8YELL3/4ABASER1U3/4GYM3ZO3/WAITE5OR2J/10OI2A/3QUOIT1PINNER/"      \
  "4RENEGADE2P AIINOOU/CDIOST? 392/450 0 -lex CSW24"

#define VIA_IS_MOVER_TURN_CGP                                                  \
  "cgp 5U4OHMIC/5N3WREATH/5T4FAX2/5i3B1VIA1/5N3L1E3/5G2VELDT2/5E3S5/"          \
  "5DREKS1F3/8YELL3/4ABASER1U3/4GYM3ZO3/WAITE5OR2J/10OI2A/3QUOIT1PINNER/"      \
  "4RENEGADE2P CDIOST?/AIINOOU 450/392 0 -lex CSW24"

void test_via_mover_must_bingo_every_depth(void) {
  // Mover (CDIOST?) on turn after VIA + (implicit opp pass). Bag empty.
  // Mover MUST play DISCO(U)nT 167 going out, value +181, at every depth.
  // The bingo terminates the game so depth-1 already suffices; depths 4-6
  // search the same tree but get pruned quickly when the leaf finds the
  // going-out bonus. Test plies 1..3 (deeper plies just confirm the same
  // answer at much higher wall-time cost).
  for (int plies = 1; plies <= 3; plies++) {
    char settings[128];
    (void)snprintf(settings, sizeof(settings),
                   "set -s1 score -s2 score -threads 6 -eplies %d "
                   "-ttfraction 0.05",
                   plies);
    test_single_endgame(settings, VIA_IS_MOVER_TURN_CGP,
                        DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
                        ERROR_STATUS_SUCCESS, 181, false, 0);
  }
}

// Cheap in-suite smoke for the VIA mover-must-bingo regression: a single 1-ply
// solve (1 thread, no PV display) of the mover-on-turn position. The going-out
// DISCO(U)nT bingo terminates the game at depth 1, so this is sub-second and
// safe for run_all; the full plies-1..3 sweep with display stays on-demand
// (viamover).
void test_via_mover_bingo_one_ply(void) {
  Config *config = config_create_or_die(
      "set -s1 score -s2 score -threads 1 -eplies 1 -ttfraction 0.05");
  load_and_exec_config_or_die(config, VIA_IS_MOVER_TURN_CGP);

  EndgameArgs args = {0};
  args.thread_control = config_get_thread_control(config);
  args.game = config_get_game(config);
  args.plies = config_get_endgame_plies(config);
  args.tt_fraction_of_mem = config_get_tt_fraction_of_mem(config);
  args.initial_small_move_arena_size = DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE;
  args.num_threads = 1;
  args.use_heuristics = true;
  args.forced_pass_bypass = true;
  args.num_top_moves = 1;
  args.seed = 42;

  EndgameResults *results = config_get_endgame_results(config);
  ErrorStack *error_stack = error_stack_create();
  EndgameCtx *ctx = NULL;
  endgame_solve(&ctx, &args, results, error_stack);
  assert(error_stack_is_empty(error_stack));

  const PVLine *pv = endgame_results_get_pvline(results, ENDGAME_RESULT_BEST);
  assert(pv->score == 181); // mover plays DISCO(U)nT going out, value +181
  assert(!small_move_is_pass(&pv->moves[0]));

  endgame_ctx_destroy(ctx);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

// Stress test: post-VIA opp-on-turn position with many randomized short
// time cutoffs. Asserts that even when IDS is interrupted mid-iteration,
// the returned result is "reasonable" — concretely, opp's value must not
// exceed 0 (opp's optimal play LOSES spread in this position; if the
// solver reports opp gains spread, that's the depth-3 IDS glitch).
void test_via_interrupted_reasonable_under_time_pressure(void) {
  // Focus on short tlim (0.05s - 0.8s): bug fires reliably when IDS gets
  // interrupted before depth-3 or depth-4 fully resolves. Earlier sweep
  // with [0.3, 3.0] range hit the glitch ~35% of the time, mostly at the
  // bottom of the range. Tighter range here both repros more often and
  // keeps the test fast.
  const int N_ITERATIONS = 100;
  const double TLIM_MIN = 0.05;
  const double TLIM_MAX = 0.8;
  // Fixed seed for reproducibility across runs.
  uint64_t rng = 0xdeadbeefULL;
  int n_glitched = 0;
  int n_finished = 0;
  int n_interrupted_no_result = 0;
  for (int iter = 0; iter < N_ITERATIONS; iter++) {
    // xorshift for a deterministic pseudo-random tlim in [TLIM_MIN, TLIM_MAX].
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    double frac = (double)(rng & 0xFFFFFFU) / (double)(0x1000000);
    double tlim = TLIM_MIN + frac * (TLIM_MAX - TLIM_MIN);

    Config *config =
        config_create_or_die("set -s1 score -s2 score -threads 6 -eplies 25");
    load_and_exec_config_or_die(config, VIA_IS_OPP_TURN_CGP);

    EndgameCtx *endgame_ctx = NULL;
    Game *game = config_get_game(config);
    EndgameArgs endgame_args = {0};
    endgame_args.thread_control = config_get_thread_control(config);
    endgame_args.game = game;
    endgame_args.plies = 25;
    endgame_args.tt_fraction_of_mem = config_get_tt_fraction_of_mem(config);
    endgame_args.initial_small_move_arena_size =
        DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE;
    endgame_args.num_threads = 6;
    endgame_args.use_heuristics = true;
    endgame_args.forced_pass_bypass = true;
    endgame_args.enable_pv_display = true;
    endgame_args.num_top_moves = 1;
    endgame_args.seed = 42;
    endgame_args.soft_time_limit = tlim;
    endgame_args.hard_time_limit = tlim;
    endgame_args.external_deadline_ns =
        ctimer_monotonic_ns() + (int64_t)(tlim * 1.0e9);

    EndgameResults *endgame_results = config_get_endgame_results(config);
    ErrorStack *error_stack = error_stack_create();
    endgame_solve(&endgame_ctx, &endgame_args, endgame_results, error_stack);

    if (!error_stack_is_empty(error_stack)) {
      n_interrupted_no_result++;
      printf("  iter %2d tlim=%.3fs: error during solve (acceptable)\n", iter,
             tlim);
      error_stack_reset(error_stack);
    } else {
      const PVLine *pv =
          endgame_results_get_pvline(endgame_results, ENDGAME_RESULT_BEST);
      int32_t value = pv->score;
      bool is_pass = small_move_is_pass(&pv->moves[0]);
      int depth =
          endgame_results_get_depth(endgame_results, ENDGAME_RESULT_BEST);
      bool glitched = (value > 0);
      if (glitched) {
        n_glitched++;
      }
      n_finished++;
      // Render the first PV move as a readable string.
      char move_str[64] = {0};
      if (is_pass) {
        (void)snprintf(move_str, sizeof(move_str), "pass");
      } else {
        Move first_move;
        small_move_to_move(&first_move, &pv->moves[0], game_get_board(game));
        StringBuilder *sb = string_builder_create();
        string_builder_add_move(sb, game_get_board(game), &first_move,
                                game_get_ld(game), true);
        (void)snprintf(move_str, sizeof(move_str), "%s",
                       string_builder_peek(sb));
        string_builder_destroy(sb);
      }
      printf("  iter %3d tlim=%.3fs depth=%d move=%-22s value=%+4d %s\n", iter,
             tlim, depth, move_str, value, glitched ? "*** GLITCHED ***" : "");
    }

    error_stack_destroy(error_stack);
    endgame_ctx_destroy(endgame_ctx);
    config_destroy(config);
  }
  printf("VIA-interrupt stress: %d iterations, %d finished, %d errored, "
         "%d glitched (value > 0)\n",
         N_ITERATIONS, n_finished, n_interrupted_no_result, n_glitched);
  // Opp's optimal play in this position LOSES spread (~ -78). Any
  // "finished" result reporting value > 0 is the depth-3 IDS glitch
  // (pass-pass returning value = +2). Tolerate 0 glitches.
  assert(n_glitched == 0);
}

void test_via_opp_must_block_every_depth(void) {
  // Opp (AIINOOU) on turn after mover played VIA. Bag empty, mover holds
  // CDIOST? with the DISCO(U)nT bingo threat at 1A through the U at 1F.
  // Opp's optimal play is 2B ONIO(N) 10 (or equivalent), which puts an O
  // at 2B forming "IO" below 1B and blocking the bingo column. Mover then
  // finds B7 STICc(A)DO 82 bingo instead (blank as c). Value from opp's
  // POV is -78, opp's first move scores 10 and is not pass. Test plies
  // 1..3 (deeper plies confirm the same answer at much higher cost).
  for (int plies = 1; plies <= 3; plies++) {
    char settings[128];
    (void)snprintf(settings, sizeof(settings),
                   "set -s1 score -s2 score -threads 6 -eplies %d "
                   "-ttfraction 0.05",
                   plies);
    test_single_endgame(settings, VIA_IS_OPP_TURN_CGP,
                        DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
                        ERROR_STATUS_SUCCESS, -78, false, 0);
  }
}

// before_search_callback tests
// ---------------------------------------------------------------------------

typedef struct BeforeSearchCaptured {
  int call_count;
  int initial_move_count;
  int initial_spread;
  int solving_player;
  // first move's tiny_move + estimated_value (lets us verify the array is
  // sorted descending by static estimate at the moment of firing)
  uint64_t first_move_tiny;
  int32_t first_move_estimate;
  // sortedness check across all received moves
  bool sorted_desc_by_estimate;
} BeforeSearchCaptured;

static void capture_before_search_cb(const Game *game,
                                     const SmallMove *initial_moves,
                                     int initial_move_count, int initial_spread,
                                     int solving_player, void *user_data) {
  (void)game;
  BeforeSearchCaptured *c = (BeforeSearchCaptured *)user_data;
  c->call_count++;
  c->initial_move_count = initial_move_count;
  c->initial_spread = initial_spread;
  c->solving_player = solving_player;
  if (initial_move_count > 0) {
    c->first_move_tiny = initial_moves[0].tiny_move;
    c->first_move_estimate = small_move_get_estimated_value(&initial_moves[0]);
  }
  c->sorted_desc_by_estimate = true;
  for (int i = 1; i < initial_move_count; i++) {
    if (small_move_get_estimated_value(&initial_moves[i]) >
        small_move_get_estimated_value(&initial_moves[i - 1])) {
      c->sorted_desc_by_estimate = false;
      break;
    }
  }
}

// Asserts the new before_search_callback fires exactly once per solve, with
// initial_move_count > 0, sorted descending by estimated_value, and that the
// polled progress atomics show root_moves_total == initial_move_count and
// root_moves_completed == 0 immediately at callback time (so a UI polling
// the atomics can render a coherent d=0 leaderboard from the same snapshot
// the callback delivered).
static atomic_int g_atomic_check_state_total;
static atomic_int g_atomic_check_state_completed;
static atomic_int g_atomic_check_state_depth;
static EndgameCtx **g_atomic_check_ctx_pp;

static void capture_and_poll_before_search_cb(
    const Game *game, const SmallMove *initial_moves, int initial_move_count,
    int initial_spread, int solving_player, void *user_data) {
  capture_before_search_cb(game, initial_moves, initial_move_count,
                           initial_spread, solving_player, user_data);
  // Poll the public progress atomics from inside the callback so we can
  // assert UI-visible state agrees with the callback args at the same
  // moment in time.
  int cur_depth = 0;
  int done = 0;
  int total = 0;
  int dummy_a = 0;
  int dummy_b = 0;
  endgame_ctx_get_progress(*g_atomic_check_ctx_pp, &cur_depth, &done, &total,
                           &dummy_a, &dummy_b);
  atomic_store(&g_atomic_check_state_depth, cur_depth);
  atomic_store(&g_atomic_check_state_completed, done);
  atomic_store(&g_atomic_check_state_total, total);
}

void test_before_search_callback(void) {
  Config *config = config_create_or_die("set -s1 score -s2 score");
  load_and_exec_config_or_die(
      config, "cgp 9A1PIXY/9S1L3/2ToWNLETS1O3/9U1DA1R/3GERANIAL1U1I/9g2T1C/"
              "8WE2OBI/6EMU4ON/6AID3GO1/5HUN4ET1/4ZA1T4ME1/1Q1FAKEY3JOES/"
              "FIVE1E5IT1C/5SPORRAN2A/6ORE2N2D BGIV/DEHILOR 384/389 0 "
              "-lex NWL20");

  EndgameCtx *ctx = NULL;
  BeforeSearchCaptured captured = {0};
  atomic_store(&g_atomic_check_state_total, -1);
  atomic_store(&g_atomic_check_state_completed, -1);
  atomic_store(&g_atomic_check_state_depth, -1);
  g_atomic_check_ctx_pp = &ctx;

  EndgameArgs args = {0};
  args.thread_control = config_get_thread_control(config);
  args.game = config_get_game(config);
  args.plies = 3;
  args.tt_fraction_of_mem = config_get_tt_fraction_of_mem(config);
  args.initial_small_move_arena_size = DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE;
  args.num_threads = 4;
  args.num_top_moves = 1;
  args.use_heuristics = true;
  args.forced_pass_bypass = true;
  args.enable_pv_display = true;
  args.seed = 42;
  args.before_search_callback = capture_and_poll_before_search_cb;
  args.before_search_callback_data = &captured;

  EndgameResults *results = config_get_endgame_results(config);
  ErrorStack *error_stack = error_stack_create();
  endgame_solve(&ctx, &args, results, error_stack);
  assert(error_stack_is_empty(error_stack));

  assert(captured.call_count == 1);
  assert(captured.initial_move_count > 0);
  assert(captured.solving_player == 0);
  assert(captured.first_move_tiny != 0); // not PASS at slot 0
  assert(captured.sorted_desc_by_estimate);

  // Polled atomics seen from inside the callback must match the callback's
  // own view of the candidate count, with no prior depth started.
  const int polled_total = atomic_load(&g_atomic_check_state_total);
  const int polled_completed = atomic_load(&g_atomic_check_state_completed);
  const int polled_depth = atomic_load(&g_atomic_check_state_depth);
  assert(polled_total == captured.initial_move_count);
  assert(polled_completed == 0);
  assert(polled_depth == 0);

  endgame_ctx_destroy(ctx);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

// ---------------------------------------------------------------------------
// On-demand: streaming-progress test
// ---------------------------------------------------------------------------

// Two streaming modes for the on-demand demo. Both share the same
// reader/printer thread; the difference is which signal source the
// reader includes in each frame.
typedef enum {
  // Poll the ply2 atomics that are already exposed by
  // endgame_ctx_get_progress. Adds intra-root sub-progress for the
  // first root move's children.
  STREAM_MODE_POLL_PLY2,
  // Use the per_root_move_callback to track the last root completed.
  // Gives per-root resolution across the whole top level, so the
  // reader can describe live root-by-root completions.
  STREAM_MODE_CALLBACK_PER_ROOT,
} StreamMode;

// Shared state between the main solver thread and the reader/printer thread.
// All cross-thread fields are either atomic_* or guarded by `mutex`.
typedef struct ProgressStream {
  EndgameCtx **ctx_pp;        // populated by endgame_solve when it creates ctx
  StreamMode mode;            // selects which signal the reader prints
  cpthread_mutex_t mutex;     // guards the snapshot fields below
  bool seen_initial;          // before_search_callback has fired
  int last_completed_depth;   // most recent per_ply_callback's depth (0 = none)
  int32_t last_completed_val; // and value
  int initial_move_count;     // from before_search_callback
  // Per-root callback snapshot (mode == STREAM_MODE_CALLBACK_PER_ROOT)
  int last_root_depth;
  int last_root_idx;
  int32_t last_root_value;
  uint64_t last_root_tiny;
  int per_root_calls;
  int64_t solve_start_ns;
  atomic_int should_stop;   // main thread sets to 1 when endgame_solve returns
  int frames_printed;       // bumped by the reader; used so we can verify
                            // the test actually streamed something
  int live_pv_change_count; // distinct live PVs the reader observed
  int live_top_k_change_count; // distinct top-K snapshots observed
  uint64_t last_topk_hash;     // hash of last top-K snapshot seen
} ProgressStream;

static void stream_before_search_cb(const Game *game,
                                    const SmallMove *initial_moves,
                                    int initial_move_count, int initial_spread,
                                    int solving_player, void *user_data) {
  (void)game;
  (void)initial_moves;
  (void)initial_spread;
  (void)solving_player;
  ProgressStream *s = (ProgressStream *)user_data;
  cpthread_mutex_lock(&s->mutex);
  s->seen_initial = true;
  s->initial_move_count = initial_move_count;
  // Start the elapsed-time clock here. Per-iteration setup before this
  // point (TT memset, KWG pruning, worker pool spinup) is bookkeeping
  // the user doesn't care about; only "time spent on actual estimates
  // and search" is interesting to display.
  s->solve_start_ns = ctimer_monotonic_ns();
  cpthread_mutex_unlock(&s->mutex);
}

static void stream_per_ply_cb(int depth, int32_t value,
                              const struct PVLine *pv_line,
                              const struct Game *game,
                              const struct PVLine *ranked_pvs,
                              int num_ranked_pvs, void *user_data) {
  (void)pv_line;
  (void)game;
  (void)ranked_pvs;
  (void)num_ranked_pvs;
  ProgressStream *s = (ProgressStream *)user_data;
  cpthread_mutex_lock(&s->mutex);
  s->last_completed_depth = depth;
  s->last_completed_val = value;
  cpthread_mutex_unlock(&s->mutex);
}

static void stream_per_root_cb(int depth, int root_index,
                               const struct SmallMove *move, int32_t value,
                               void *user_data) {
  ProgressStream *s = (ProgressStream *)user_data;
  cpthread_mutex_lock(&s->mutex);
  s->last_root_depth = depth;
  s->last_root_idx = root_index;
  s->last_root_value = value;
  s->last_root_tiny = move->tiny_move;
  s->per_root_calls++;
  cpthread_mutex_unlock(&s->mutex);
}

static void *stream_reader_thread(void *arg) {
  ProgressStream *s = (ProgressStream *)arg;
  uint64_t prev_nodes = 0;
  int64_t prev_nodes_ns = 0;
  // Track how often the live PV's content actually changes between
  // poll frames (not just the seqlock seq, the displayed bytes).
  uint64_t last_pv_hash = 0;
  int live_pv_change_count = 0;
  while (!atomic_load(&s->should_stop)) {
    const EndgameCtx *ctx = *s->ctx_pp;
    cpthread_mutex_lock(&s->mutex);
    const bool seen_initial = s->seen_initial;
    const int last_depth = s->last_completed_depth;
    const int32_t last_val = s->last_completed_val;
    const int initial_count = s->initial_move_count;
    const int last_root_depth = s->last_root_depth;
    const int last_root_idx = s->last_root_idx;
    const int32_t last_root_value = s->last_root_value;
    const uint64_t last_root_tiny = s->last_root_tiny;
    const int per_root_calls = s->per_root_calls;
    const int64_t solve_start_ns = s->solve_start_ns;
    cpthread_mutex_unlock(&s->mutex);

    int cur_depth = 0;
    int done = 0;
    int total = 0;
    int ply2_done = 0;
    int ply2_total = 0;
    uint64_t nodes = 0;
    uint64_t line[MAX_SEARCH_DEPTH];
    int line_len = 0;
    uint64_t live_pv[MAX_SEARCH_DEPTH];
    int live_pv_len = 0;
    int32_t live_pv_value = 0;
    EndgameLivePvSnapshot live_top_k[10] = {0};
    int live_top_k_n = 0;
    if (ctx != NULL) {
      endgame_ctx_get_progress(ctx, &cur_depth, &done, &total, &ply2_done,
                               &ply2_total);
    }
    // The worker-array accessors (nodes/current-line/live-PV/top-K) read
    // ctx->workers, which endgame_solve's prepare_workers may free and rebuild
    // when the lexicon changes between solves. Only poll them once this solve's
    // before_search_callback has fired (seen_initial) — by then prepare_workers
    // has finished and the pool is stable for the duration of the search. This
    // mirrors the contract that the accessors are valid only during an active
    // solve.
    if (ctx != NULL && seen_initial) {
      nodes = endgame_ctx_get_nodes_searched(ctx);
      line_len = endgame_ctx_get_current_line(ctx, 0, line, MAX_SEARCH_DEPTH);
      live_pv_len = endgame_ctx_get_live_pv(ctx, 0, live_pv, MAX_SEARCH_DEPTH,
                                            &live_pv_value);
      live_top_k_n = endgame_ctx_get_live_top_k_pvs(ctx, 0, live_top_k, 10);
    }
    // Hash the live PV (length + moves + value) so we can count the
    // number of distinct snapshots seen by the reader. This isn't the
    // engine-side update count — it's "how often a polling display
    // would observe a change."
    uint64_t pv_hash = (uint64_t)live_pv_len * 0x9E3779B97F4A7C15ULL;
    for (int i = 0; i < live_pv_len; i++) {
      pv_hash ^=
          live_pv[i] + 0x9E3779B97F4A7C15ULL + (pv_hash << 6) + (pv_hash >> 2);
    }
    pv_hash ^= (uint64_t)(uint32_t)live_pv_value;
    if (pv_hash != last_pv_hash) {
      live_pv_change_count++;
      last_pv_hash = pv_hash;
      s->live_pv_change_count = live_pv_change_count;
    }
    // Hash the multi-PV top-K snapshot (length + each entry's
    // root_tiny + value + continuation) to count how often the
    // observable leaderboard changes between polls.
    uint64_t topk_hash = (uint64_t)live_top_k_n * 0x9E3779B97F4A7C15ULL;
    for (int i = 0; i < live_top_k_n; i++) {
      topk_hash ^=
          live_top_k[i].root_tiny + (topk_hash << 6) + (topk_hash >> 2);
      topk_hash ^= (uint64_t)(uint32_t)live_top_k[i].value;
      for (int j = 0; j < live_top_k[i].continuation_len; j++) {
        topk_hash ^= live_top_k[i].continuation_tiny[j] + (topk_hash << 6) +
                     (topk_hash >> 2);
      }
    }
    if (topk_hash != s->last_topk_hash) {
      s->live_top_k_change_count++;
      s->last_topk_hash = topk_hash;
    }
    const int64_t now_ns = ctimer_monotonic_ns();
    double knodes_per_sec = 0.0;
    if (prev_nodes_ns > 0 && now_ns > prev_nodes_ns && nodes >= prev_nodes) {
      knodes_per_sec = (double)(nodes - prev_nodes) /
                       ((double)(now_ns - prev_nodes_ns) / 1.0e6);
    }
    prev_nodes = nodes;
    prev_nodes_ns = now_ns;

    // Common prefix: timing + d=0 status + last completed depth + root
    // counter. Mode-specific suffix appended below.  Pre-d=0 frames have
    // no meaningful elapsed value (the timer is started by the d=0
    // callback to exclude setup overhead like TT allocation).
    char prefix[160];
    if (!seen_initial) {
      (void)snprintf(prefix, sizeof(prefix),
                     "  [    --ms] (waiting for d=0 callback)");
    } else {
      const double elapsed_ms = (double)(now_ns - solve_start_ns) / 1.0e6;
      if (last_depth == 0) {
        (void)snprintf(
            prefix, sizeof(prefix),
            "  [%6.0fms] d=0 published (n=%d) cur_depth=%d searching %d/%d",
            elapsed_ms, initial_count, cur_depth, done, total);
      } else {
        (void)snprintf(prefix, sizeof(prefix),
                       "  [%6.0fms] last_completed=d%d val=%d "
                       "cur_depth=%d searching %d/%d",
                       elapsed_ms, last_depth, last_val, cur_depth, done,
                       total);
      }
    }

    // Format the live current-line as comma-separated tiny_moves.
    char line_str[256];
    int written = 0;
    written += snprintf(line_str + written, sizeof(line_str) - written, "[");
    for (int i = 0; i < line_len && written < (int)sizeof(line_str) - 32; i++) {
      written +=
          snprintf(line_str + written, sizeof(line_str) - written, "%s0x%llx",
                   i == 0 ? "" : ",", (unsigned long long)line[i]);
    }
    (void)snprintf(line_str + written, sizeof(line_str) - written, "]");

    // Always-on signals appended to every frame: nodes/sec (smoothed
    // across the last poll interval) and the live current-line snapshot
    // from worker thread 0. Either alone is enough to prove the engine
    // is alive during a long single-root subtree where no other signal
    // updates.
    char pv_str[256];
    int pv_written = 0;
    pv_written +=
        snprintf(pv_str + pv_written, sizeof(pv_str) - pv_written, "[");
    for (int i = 0; i < live_pv_len && pv_written < (int)sizeof(pv_str) - 32;
         i++) {
      pv_written +=
          snprintf(pv_str + pv_written, sizeof(pv_str) - pv_written, "%s0x%llx",
                   i == 0 ? "" : ",", (unsigned long long)live_pv[i]);
    }
    (void)snprintf(pv_str + pv_written, sizeof(pv_str) - pv_written, "]");

    // Compact "top-K" tail: show first-move tiny + value for each slot,
    // then "+continuation_len" for the longest continuation. Reader
    // verifies the leaderboard fills in as roots complete.
    char topk_str[128];
    int topk_written = 0;
    topk_written +=
        snprintf(topk_str + topk_written, sizeof(topk_str) - topk_written,
                 "%d{", live_top_k_n);
    for (int i = 0;
         i < live_top_k_n && topk_written < (int)sizeof(topk_str) - 32; i++) {
      topk_written +=
          snprintf(topk_str + topk_written, sizeof(topk_str) - topk_written,
                   "%s0x%llx=%d+%d", i == 0 ? "" : ",",
                   (unsigned long long)live_top_k[i].root_tiny,
                   live_top_k[i].value, live_top_k[i].continuation_len);
    }
    (void)snprintf(topk_str + topk_written, sizeof(topk_str) - topk_written,
                   "}");

    if (s->mode == STREAM_MODE_POLL_PLY2) {
      printf("%s | ply2 %d/%d | %.0fk nps | t0_line %s | "
             "live_pv val=%d %s | top_k %s\n",
             prefix, ply2_done, ply2_total, knodes_per_sec, line_str,
             live_pv_value, pv_str, topk_str);
    } else {
      if (per_root_calls == 0) {
        printf("%s | per_root: 0 calls | %.0fk nps | t0_line %s | "
               "live_pv val=%d %s | top_k %s\n",
               prefix, knodes_per_sec, line_str, live_pv_value, pv_str,
               topk_str);
      } else {
        printf("%s | per_root: %d calls, last d%d idx=%d val=%d tiny=0x%llx | "
               "%.0fk nps | t0_line %s | live_pv val=%d %s | top_k %s\n",
               prefix, per_root_calls, last_root_depth, last_root_idx,
               last_root_value, (unsigned long long)last_root_tiny,
               knodes_per_sec, line_str, live_pv_value, pv_str, topk_str);
      }
    }
    (void)fflush(stdout);
    s->frames_printed++;
    ctime_nap(0.016); // ~60fps
  }
  return NULL;
}

void test_endgame_progress_stream(void) {
  // Hand-picked positions tuned so each solve runs for ~1-3 seconds with
  // multiple completed depths visible in the stream.  Don't crank
  // eplies higher: deep searches on a wide root (n>500) blow past 30s
  // per iteration and the test stops being useful as a demo.
  static const struct {
    const char *cgp;
    const char *lex;
    int eplies;
  } positions[] = {
      // Tight 7-tile endgame, ~60-100 root moves, completes through d=5
      // quickly enough to show many depth transitions in the stream.
      {"9A1PIXY/9S1L3/2ToWNLETS1O3/9U1DA1R/3GERANIAL1U1I/9g2T1C/8WE2OBI/"
       "6EMU4ON/6AID3GO1/5HUN4ET1/4ZA1T4ME1/1Q1FAKEY3JOES/FIVE1E5IT1C/"
       "5SPORRAN2A/6ORE2N2D BGIV/DEHILOR 384/389 0",
       "NWL20", 6},
      // Stuck-tile (Z) position, asymmetric racks, slower per-ply.
      {"GATELEGs1POGOED/R4MOOLI3X1/AA10U2/YU4BREDRIN2/1TITULE3E1IN1/"
       "1E4N3c1BOK/1C2O4CHARD1/QI1FLAWN2E1OE1/IS2E1HIN1A1W2/"
       "1MOTIVATE1T1S2/1S2N5S4/3PERJURY5/15/15/15 FV/AADIZ 442/388 0",
       "CSW21", 5},
  };
  static const int npos = (int)(sizeof(positions) / sizeof(positions[0]));
  static const StreamMode modes[] = {STREAM_MODE_POLL_PLY2,
                                     STREAM_MODE_CALLBACK_PER_ROOT};
  static const int nmodes = (int)(sizeof(modes) / sizeof(modes[0]));
  static const char *mode_labels[] = {"poll_ply2", "callback_per_root"};

  // Use system time as the RNG seed only for choosing thread counts.
  // (Position and mode are cycled deterministically so the test always
  // exercises every combination.)
  XoshiroPRNG *prng = prng_create((uint64_t)ctime_get_current_time());

  // Allocate the EndgameCtx (and its transposition table) once, outside
  // the iteration loop. Subsequent iterations reuse it, so the
  // ~200ms one-off TT memset doesn't get counted into per-iteration
  // streaming output. The elapsed-time clock in each iteration is
  // started by stream_before_search_cb, so even the first iteration's
  // streaming numbers exclude setup overhead.
  EndgameCtx *ctx = NULL;
  {
    // Warmup: one quick solve with eplies=1 to force ctx + TT allocation.
    // Its stream output is suppressed (no reader thread); we just care
    // about getting the TT on the heap.
    Config *warmup_config = config_create_or_die("set -s1 score -s2 score");
    char warmup_cmd[1024];
    (void)snprintf(warmup_cmd, sizeof(warmup_cmd), "cgp %s -lex %s",
                   positions[0].cgp, positions[0].lex);
    load_and_exec_config_or_die(warmup_config, warmup_cmd);
    EndgameArgs warmup_args = {0};
    warmup_args.thread_control = config_get_thread_control(warmup_config);
    warmup_args.game = config_get_game(warmup_config);
    warmup_args.plies = 1;
    warmup_args.tt_fraction_of_mem =
        config_get_tt_fraction_of_mem(warmup_config);
    warmup_args.initial_small_move_arena_size =
        DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE;
    warmup_args.num_threads = 1;
    warmup_args.num_top_moves = 1;
    warmup_args.use_heuristics = true;
    warmup_args.forced_pass_bypass = true;
    warmup_args.enable_pv_display = true;
    warmup_args.seed = 1;
    EndgameResults *warmup_results = config_get_endgame_results(warmup_config);
    ErrorStack *warmup_err = error_stack_create();
    endgame_solve(&ctx, &warmup_args, warmup_results, warmup_err);
    assert(error_stack_is_empty(warmup_err));
    error_stack_destroy(warmup_err);
    config_destroy(warmup_config);
    printf("(warmup complete; TT pre-allocated)\n");
  }

  const int total_iterations = npos * nmodes;
  int iter = 0;
  for (int pos_idx = 0; pos_idx < npos; pos_idx++) {
    for (int mode_idx = 0; mode_idx < nmodes; mode_idx++) {
      iter++;
      const int threads = 2 + (int)prng_get_random_number(prng, 5); // 2..6
      const StreamMode mode = modes[mode_idx];

      Config *config = config_create_or_die("set -s1 score -s2 score");
      char cmd[1024];
      (void)snprintf(cmd, sizeof(cmd), "cgp %s -lex %s", positions[pos_idx].cgp,
                     positions[pos_idx].lex);
      load_and_exec_config_or_die(config, cmd);

      printf(
          "\n=== iter %d/%d: pos=%d mode=%s threads=%d plies=%d lex=%s ===\n",
          iter, total_iterations, pos_idx, mode_labels[mode_idx], threads,
          positions[pos_idx].eplies, positions[pos_idx].lex);

      ProgressStream stream = {0};
      cpthread_mutex_init(&stream.mutex);
      atomic_store(&stream.should_stop, 0);
      stream.ctx_pp = &ctx;
      stream.mode = mode;
      // solve_start_ns will be filled in by stream_before_search_cb.

      EndgameArgs args = {0};
      args.thread_control = config_get_thread_control(config);
      args.game = config_get_game(config);
      args.plies = positions[pos_idx].eplies;
      args.tt_fraction_of_mem = config_get_tt_fraction_of_mem(config);
      args.initial_small_move_arena_size =
          DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE;
      args.num_threads = threads;
      args.num_top_moves = 5;
      args.use_heuristics = true;
      args.forced_pass_bypass = true;
      args.enable_pv_display = true;
      args.seed = 42;
      args.before_search_callback = stream_before_search_cb;
      args.before_search_callback_data = &stream;
      args.per_ply_callback = stream_per_ply_cb;
      args.per_ply_callback_data = &stream;
      // Only install the per-root callback when the mode actually wants it,
      // so each mode runs with only its own signal active and the comparison
      // is honest.
      if (mode == STREAM_MODE_CALLBACK_PER_ROOT) {
        args.per_root_move_callback = stream_per_root_cb;
        args.per_root_move_callback_data = &stream;
      }

      // Clear the TT before each timed iteration so each one searches
      // from a cold cache. This costs a 2GB memset (~200ms) on this
      // host but happens BEFORE the reader thread is spawned and before
      // the timer is started by the d=0 callback, so it isn't counted
      // in any displayed elapsed time.
      endgame_ctx_clear_transposition_table(ctx);

      cpthread_t reader;
      cpthread_create(&reader, stream_reader_thread, &stream);

      EndgameResults *results = config_get_endgame_results(config);
      ErrorStack *error_stack = error_stack_create();
      endgame_solve(&ctx, &args, results, error_stack);
      assert(error_stack_is_empty(error_stack));

      atomic_store(&stream.should_stop, 1);
      cpthread_join(reader);

      printf("  [done] frames_printed=%d final_completed_depth=%d "
             "per_root_calls=%d live_pv_changes=%d "
             "live_top_k_changes=%d\n",
             stream.frames_printed, stream.last_completed_depth,
             stream.per_root_calls, stream.live_pv_change_count,
             stream.live_top_k_change_count);
      assert(stream.frames_printed > 0);
      assert(stream.seen_initial);
      if (mode == STREAM_MODE_CALLBACK_PER_ROOT) {
        // Should fire at least once per completed depth.
        assert(stream.per_root_calls >= stream.last_completed_depth);
      }

      error_stack_destroy(error_stack);
      config_destroy(config);
    }
  }
  endgame_ctx_destroy(ctx);
  prng_destroy(prng);
}

void test_endgame(void) {
  test_before_search_callback();
  test_single_pv_display();
  test_ctx_reuse();
  test_solve_standard();
  test_very_deep();
  test_small_arena_realloc();
  test_pass_first();
  test_nonempty_bag();
  // 2-lexicon endgame tests (TWL98 vs CSW24, QI-relevant)
  test_2lex_ignorant();
  test_2lex_informed();
  test_endgame_interrupt();
  // Cheap (sub-second) regression of the VIA mover-must-bingo case. The full
  // VIA depth/stress sweeps (viamover / viaopp / viastress) are on-demand only
  // — viamover re-solves plies 1..3 with display (~14s) and viastress runs 100
  // timed iterations — so they are not invoked here from the run_all suite.
  test_via_mover_bingo_one_ply();
  //  Uncomment out more of these tests once we add more optimizations,
  //  and/or if we can run the endgame tests in release mode.
  // test_vs_joey();
}

void test_monster_q(void) {
  Config *config = config_create_or_die("set -s1 score -s2 score -eplies 6 "
                                        "-ttfraction 0.5");
  load_and_exec_config_or_die(config,
                              "cgp "
                              "5LEX1AFFORD/3SNOWIER1Y3/2V8T3/1DO6J1T3/1AG6OKE3/"
                              "1U7YE3N/1B8T3E/ERICA2GARTH2V/1I2MIAOW1L3U/"
                              "PE6AZIONES/OS8n4/L6DINGBATS/I14/COULEE9/"
                              "E3HARN7 "
                              "ADEIIU?/MNPQRT 369/399 0 -lex CSW21;");

  EndgameCtx *endgame_ctx = NULL;
  Game *game = config_get_game(config);
  Timer timer;
  ctimer_start(&timer);

  EndgameArgs endgame_args = {0};
  endgame_args.thread_control = config_get_thread_control(config);
  endgame_args.game = game;
  endgame_args.plies = 6;
  endgame_args.tt_fraction_of_mem = 0.5;
  endgame_args.initial_small_move_arena_size =
      DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE;
  endgame_args.num_threads = 6;
  endgame_args.use_heuristics = true;
  endgame_args.forced_pass_bypass = true;
  endgame_args.enable_pv_display = true;
  endgame_args.num_top_moves = 1;
  endgame_args.per_ply_callback = print_pv_callback;
  endgame_args.per_ply_callback_data = &timer;
  endgame_args.seed = 42;

  EndgameResults *endgame_results = config_get_endgame_results(config);
  ErrorStack *error_stack = error_stack_create();

  StringBuilder *game_sb = string_builder_create();
  GameStringOptions *gso = game_string_options_create_default();
  string_builder_add_game(game, NULL, gso, NULL, game_sb);
  printf("\n%s\n", string_builder_peek(game_sb));
  string_builder_destroy(game_sb);
  game_string_options_destroy(gso);

  printf("Solving %d-ply endgame with %d threads, ttfraction=%.1f...\n",
         endgame_args.plies, endgame_args.num_threads,
         endgame_args.tt_fraction_of_mem);
  endgame_solve(&endgame_ctx, &endgame_args, endgame_results, error_stack);
  assert(error_stack_is_empty(error_stack));

  endgame_ctx_destroy(endgame_ctx);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

void test_multi_pv(void) {
  // Test multi-PV mode: solve a 4-ply endgame requesting top 5 moves.
  // Verify we get multiple PVs back with values in descending order,
  // and the best PV matches the single-PV result.
  Config *config = config_create_or_die("set -s1 score -s2 score -eplies 4");
  load_and_exec_config_or_die(
      config, "cgp "
              "9A1PIXY/9S1L3/2ToWNLETS1O3/9U1DA1R/3GERANIAL1U1I/9g2T1C/8WE2OBI/"
              "6EMU4ON/6AID3GO1/5HUN4ET1/4ZA1T4ME1/1Q1FAKEY3JOES/FIVE1E5IT1C/"
              "5SPORRAN2A/6ORE2N2D BGIV/DEHILOR 384/389 0 -lex NWL20");

  // First: solve single-PV to get the reference best value
  EndgameCtx *endgame_ctx = NULL;
  Game *game = config_get_game(config);

  EndgameArgs endgame_args = {0};
  endgame_args.thread_control = config_get_thread_control(config);
  endgame_args.game = game;
  endgame_args.plies = 4;
  endgame_args.tt_fraction_of_mem = config_get_tt_fraction_of_mem(config);
  endgame_args.initial_small_move_arena_size =
      DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE;
  endgame_args.num_threads = 6;
  endgame_args.num_top_moves = 1;
  endgame_args.use_heuristics = true;
  endgame_args.per_ply_callback = NULL;
  endgame_args.per_ply_callback_data = NULL;
  endgame_args.seed = 42;

  EndgameResults *endgame_results = endgame_results_create();
  ErrorStack *error_stack = error_stack_create();

  endgame_solve(&endgame_ctx, &endgame_args, endgame_results, error_stack);
  assert(error_stack_is_empty(error_stack));
  const PVLine *single_pv =
      endgame_results_get_pvline(endgame_results, ENDGAME_RESULT_BEST);
  int32_t single_best_score = single_pv->score;

  endgame_ctx_destroy(endgame_ctx);

  // Now: solve multi-PV with top 5
  // Multi-PV ranked moves are reported via the per-ply callback;
  // the results struct stores only the best PV.
  endgame_ctx = NULL;
  endgame_args.num_top_moves = 5;
  Timer timer;
  ctimer_start(&timer);
  endgame_args.per_ply_callback = print_pv_and_ranked_callback;
  endgame_args.per_ply_callback_data = &timer;

  StringBuilder *game_sb = string_builder_create();
  GameStringOptions *gso = game_string_options_create_default();
  string_builder_add_game(game, NULL, gso, NULL, game_sb);
  printf("\n%s\n", string_builder_peek(game_sb));
  string_builder_destroy(game_sb);
  game_string_options_destroy(gso);

  printf("Solving %d-ply endgame with %d threads, top %d moves...\n",
         endgame_args.plies, endgame_args.num_threads,
         endgame_args.num_top_moves);

  EndgameResults *multi_results = endgame_results_create();
  endgame_solve(&endgame_ctx, &endgame_args, multi_results, error_stack);
  assert(error_stack_is_empty(error_stack));

  // Best PV should match single-PV result
  const PVLine *best_pv =
      endgame_results_get_pvline(multi_results, ENDGAME_RESULT_BEST);
  printf("Single-PV best: %d, Multi-PV best: %d\n", single_best_score,
         best_pv->score);
  assert(best_pv->score == single_best_score);
  assert(best_pv->num_moves >= 1);

  // Test string output
  char *result_str = endgame_results_get_string(multi_results, game, NULL);
  printf("Multi-PV output:\n%s\n", result_str);
  free(result_str);

  endgame_ctx_destroy(endgame_ctx);
  endgame_results_destroy(multi_results);
  endgame_results_destroy(endgame_results);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

void test_endgame_wasm(void) {
  test_solve_standard();
  test_small_arena_realloc();
  test_pass_first();
  test_nonempty_bag();
}
