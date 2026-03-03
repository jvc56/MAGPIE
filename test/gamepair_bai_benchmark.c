// Game-pair autoplay benchmark: static BAI vs nested-sim BAI.
// Usage: ./bin/magpie_test gamepairbai
//
// Plays 10 game pairs from an empty board. Each pair uses the same seed
// but swaps which player uses the static strategy vs the nested strategy.
// Every turn: generate 15 candidates → BAI with fixed time limit → play best.
// BAI uses NO threshold (BAI_THRESHOLD_NONE), only the time limit.

#include "../src/def/config_defs.h"
#include "../src/compat/ctime.h"
#include "../src/def/bai_defs.h"
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
#include "../src/str/sim_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_constants.h"
#include "test_util.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#define NUM_GAME_PAIRS 150
#define NUM_PLAYS 15
#define STATIC_PLIES 4
#define NESTED_PLIES 2
#define NUM_THREADS 10
#define TURN_TIME_LIMIT_S 5
#define ENDGAME_PLIES 25
#define ENDGAME_TIME_LIMIT_S 10.0
#define LATE_GAME_TILE_THRESHOLD 21
#define LATE_GAME_PLIES 99

// Two strategies to compare
static const FidelityLevel STRATEGY_STATIC = {
    .sample_limit = UINT64_MAX,
    .sample_minimum = 1,
    .time_limit_seconds = TURN_TIME_LIMIT_S,
    .ply_strategy = PLY_STRATEGY_STATIC,
};

static const FidelityLevel STRATEGY_NESTED = {
    .sample_limit = UINT64_MAX,
    .sample_minimum = 1,
    .time_limit_seconds = TURN_TIME_LIMIT_S,
    .ply_strategy = PLY_STRATEGY_NESTED_SIM,
    .nested_candidates = 5,
    .nested_rollouts = 8,
    .nested_plies = 2,
};

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
         rack_get_total_letters(player_get_rack(game_get_player(
             game, 1 - game_get_player_on_turn_index(game))));
}

// Play one move using the endgame solver with a time limit.
static const Move *play_endgame_turn(Game *game, MoveList *move_list,
                                     EndgameSolver *solver,
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

  endgame_solve(solver, &args, endgame_results, error_stack);

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

// Play one move using BAI with the given fidelity level.
// Returns the move played (pointer into move_list, valid until next gen).
static const Move *play_sim_turn(Game *game, MoveList *move_list,
                                 SimResults *sim_results, SimCtx **sim_ctx,
                                 WinPct *win_pcts, ThreadControl *tc,
                                 const FidelityLevel *strategy, int num_plies,
                                 ErrorStack *error_stack) {
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
    // Only one move (or pass) - just play it
    const Move *move = move_list_get_move(move_list, 0);
    play_move(move, game, NULL);
    return move;
  }

  // Build SimArgs with fixed time limit, no threshold
  SimArgs sim_args;
  sim_args_fill(
      num_plies, move_list,
      /* known_opp_rack */ NULL, win_pcts,
      /* inference_results */ NULL, tc, game,
      /* sim_with_inference */ false,
      /* use_heat_map */ false, NUM_THREADS,
      /* print_interval */ 0,
      /* max_num_display_plays */ NUM_PLAYS,
      /* max_num_display_plies */ num_plies,
      /* seed */ 0,
      /* max_iterations */ UINT64_MAX,
      /* min_play_iterations */ 1,
      /* scond */ 101.0,  // > 100 → BAI_THRESHOLD_NONE
      BAI_THRESHOLD_NONE,
      strategy->time_limit_seconds,
      BAI_SAMPLING_RULE_ROUND_ROBIN,
      /* cutoff */ 0.0,
      /* inference_args */ NULL, &sim_args);

  // Override fidelity to use our strategy
  sim_args.num_fidelity_levels = 1;
  sim_args.fidelity_levels[0] = *strategy;

  thread_control_set_status(tc, THREAD_CONTROL_STATUS_STARTED);
  simulate(&sim_args, sim_ctx, sim_results, error_stack);

  // Log sim results for all candidates
  char *sim_str = sim_results_get_string(
      game, sim_results, NUM_PLAYS, num_plies,
      /*filter_row=*/-1, /*filter_col=*/-1,
      /*prefix_mls=*/NULL, /*prefix_len=*/0,
      /*exclude_tile_placement_moves=*/false,
      /*use_ucgi_format=*/false,
      /*game_board_string=*/NULL);
  printf("    --- Sim Results ---\n%s", sim_str);
  free(sim_str);

  // Get best arm
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

// Play a full game. p0_strategy and p1_strategy control fidelity per player.
static void play_game(Game *game, MoveList *move_list, SimResults *sim_results,
                      SimCtx **sim_ctx, WinPct *win_pcts, ThreadControl *tc,
                      EndgameSolver *solver, EndgameResults *endgame_results,
                      const FidelityLevel *p0_strategy,
                      const FidelityLevel *p1_strategy,
                      int p0_plies, int p1_plies,
                      const char *p0_label, const char *p1_label,
                      int game_num, uint64_t seed, PairResults *results) {
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
    printf("    -- Before turn %d --\n%s\n", turn + 1,
           string_builder_peek(sb));

    int unseen = tiles_unseen(game);
    const Move *move;
    bool bag_empty = bag_is_empty(game_get_bag(game));

    struct timespec turn_start, turn_end;
    clock_gettime(CLOCK_MONOTONIC, &turn_start);

    if (bag_empty) {
      // Both players use endgame solver when bag is empty
      printf("    [turn %d: ENDGAME mode, unseen=%d]\n", turn + 1, unseen);
      move = play_endgame_turn(game, move_list, solver, endgame_results, tc,
                               error_stack);
      label = "ENDGM";
    } else {
      int effective_plies = plies;
      if (unseen < LATE_GAME_TILE_THRESHOLD) {
        effective_plies = LATE_GAME_PLIES;
        printf("    [turn %d: LATE-GAME mode, unseen=%d, plies=%d→%d]\n",
               turn + 1, unseen, plies, effective_plies);
      } else {
        printf("    [turn %d: SIM mode, unseen=%d, plies=%d]\n",
               turn + 1, unseen, plies);
      }
      move = play_sim_turn(game, move_list, sim_results, sim_ctx, win_pcts, tc,
                           strategy, effective_plies, error_stack);
    }

    clock_gettime(CLOCK_MONOTONIC, &turn_end);
    double turn_elapsed =
        (turn_end.tv_sec - turn_start.tv_sec) +
        (turn_end.tv_nsec - turn_start.tv_nsec) / 1e9;
    turn++;

    // Log the move played this turn
    string_builder_clear(sb);
    string_builder_add_move(sb, game_get_board(game), move,
                            game_get_ld(game), true);
    printf("    Turn %2d (%-6s): %s  [%.1fs]\n", turn, label,
           string_builder_peek(sb), turn_elapsed);

    if (!error_stack_is_empty(error_stack)) {
      printf("  ERROR on turn %d: ", turn);
      error_stack_print_and_reset(error_stack);
      break;
    }
  }

  // Print final board
  string_builder_clear(sb);
  string_builder_add_game(game, NULL, gso, NULL, sb);
  printf("%s\n", string_builder_peek(sb));
  game_string_options_destroy(gso);

  clock_gettime(CLOCK_MONOTONIC, &game_end);
  double elapsed =
      (game_end.tv_sec - game_start.tv_sec) +
      (game_end.tv_nsec - game_start.tv_nsec) / 1e9;

  int p0_score = equity_to_int(
      player_get_score(game_get_player(game, 0)));
  int p1_score = equity_to_int(
      player_get_score(game_get_player(game, 1)));

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

  printf("  Game %2d: %s(%d) vs %s(%d) → %s  [%d turns, %.1fs]\n",
         game_num, p0_label, p0_score, p1_label, p1_score,
         winner_label, turn, elapsed);

  string_builder_destroy(sb);
  error_stack_destroy(error_stack);
}

void test_gamepair_bai_benchmark(void) {
  setbuf(stdout, NULL);  // Disable stdout buffering for real-time output
  printf("\n");
  printf("================================================\n");
  printf("  Game-Pair BAI Benchmark\n");
  printf("  %d game pairs, %ds/turn, %d threads\n",
         NUM_GAME_PAIRS, TURN_TIME_LIMIT_S, NUM_THREADS);
  printf("  Static(%d-ply) vs Nested(%d-ply outer, %d-ply inner, K=5 N=8)\n",
         STATIC_PLIES, NESTED_PLIES, STRATEGY_NESTED.nested_plies);
  printf("  Endgame: %d-ply, %.0fs limit | Late-game: <%d unseen → %d plies\n",
         ENDGAME_PLIES, ENDGAME_TIME_LIMIT_S,
         LATE_GAME_TILE_THRESHOLD, LATE_GAME_PLIES);
  printf("================================================\n");

  // Create config and load game data via a CGP
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 15 -plies 2 -threads 10");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);

  // Load win_pcts directly (avoid throwaway sim on empty board)
  ErrorStack *load_es = error_stack_create();
  WinPct *win_pcts = win_pct_create(config_get_data_paths(config),
                                     DEFAULT_WIN_PCT, load_es);
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
  EndgameSolver *solver = endgame_solver_create();
  EndgameResults *endgame_results = endgame_results_create();

  PairResults static_as_p0 = {0};  // static=P0, nested=P1
  PairResults nested_as_p0 = {0};  // nested=P0, static=P1

  printf("\n--- Games ---\n");
  for (int pair = 0; pair < NUM_GAME_PAIRS; pair++) {
    uint64_t seed = 9 + (uint64_t)pair;

    // Game A: Static=P0, Nested=P1
    play_game(game, move_list, sim_results, &sim_ctx, win_pcts, tc,
              solver, endgame_results,
              &STRATEGY_STATIC, &STRATEGY_NESTED,
              STATIC_PLIES, NESTED_PLIES,
              "STATIC", "NESTED", pair * 2 + 1, seed, &static_as_p0);

    // Game B: Nested=P0, Static=P1 (same seed, swapped strategies)
    play_game(game, move_list, sim_results, &sim_ctx, win_pcts, tc,
              solver, endgame_results,
              &STRATEGY_NESTED, &STRATEGY_STATIC,
              NESTED_PLIES, STATIC_PLIES,
              "NESTED", "STATIC", pair * 2 + 2, seed, &nested_as_p0);
  }

  // Aggregate: count wins by strategy, not by player position
  int static_wins = static_as_p0.p0_wins + nested_as_p0.p1_wins;
  int nested_wins = static_as_p0.p1_wins + nested_as_p0.p0_wins;
  int ties = static_as_p0.ties + nested_as_p0.ties;
  int total_games = NUM_GAME_PAIRS * 2;
  int total_turns = static_as_p0.total_turns + nested_as_p0.total_turns;
  double total_elapsed =
      static_as_p0.total_elapsed_s + nested_as_p0.total_elapsed_s;

  printf("\n================================================\n");
  printf("  RESULTS (%d games = %d pairs)\n", total_games, NUM_GAME_PAIRS);
  printf("  Static wins: %d (%.1f%%)\n", static_wins,
         100.0 * static_wins / total_games);
  printf("  Nested wins: %d (%.1f%%)\n", nested_wins,
         100.0 * nested_wins / total_games);
  printf("  Ties:        %d\n", ties);
  printf("  Avg turns/game: %.1f\n", (double)total_turns / total_games);
  printf("  Total wall time: %.1fs (%.1fs/game avg)\n",
         total_elapsed, total_elapsed / total_games);
  printf("================================================\n");

  endgame_results_destroy(endgame_results);
  endgame_solver_destroy(solver);
  sim_ctx_destroy(sim_ctx);
  sim_results_destroy(sim_results);
  move_list_destroy(move_list);
  game_destroy(game);
  win_pct_destroy(win_pcts);
  config_destroy(config);
}

// Late-game position from seed 9, Game 1 (turn 18, unseen=19).
// On-turn player listed first (was P1/NESTED in original game).
#define LATE_GAME_TEST_CGP                                                     \
  "15/1G3NIQAB5/1A7R5/1ZEL1AGILItY3/PORISM3COOED1/1N2HUIA1K5/2WHO10/"        \
  "1TOMENTA2W1FEU/VOX4TAPERER1/3G6NERAL/3UNSUITED4/3V11/15/15/15 "            \
  "ACEFNOT/BEEIRST 306/324 0 -lex CSW21;"

// Pair 11 turn 2: CEIMRTZ on turn after PURELY at 8G, trailing 0-30.
#define PAIR11_TURN2_CGP                                                       \
  "15/15/15/15/15/15/15/6PURELY3/15/15/15/15/15/15/15 "                        \
  "CEIMRTZ/AEEFGSU 0/30 0 -lex CSW21;"

// Pair 23 turn 18: EINNOTW trailing 305-340, 20 tiles unseen (13 in bag).
// Late-game position that triggered premature win percentage cutoff with
// only 1 iteration per candidate in 99-ply sims.
#define PAIR23_TURN18_CGP                                                      \
  "B2VAGUS7/IF1O1I3C5/BO1CHEQUER5/1L1E1DIGLOT4/PEHS5OY4/AYE6NED3/"          \
  "T1R1DUX4I3/1MOKIhIS3L3/2I7PA3/2ZA6AT3/1GEE6WE3/1ISO7R3/1F1N11/"          \
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
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 15 -plies 2 -threads 10");
  load_and_exec_config_or_die(config, "cgp " PAIR11_TURN2_CGP);

  ErrorStack *load_es = error_stack_create();
  WinPct *win_pcts = win_pct_create(config_get_data_paths(config),
                                     DEFAULT_WIN_PCT, load_es);
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
  printf("\n--- Running STATIC sim (%d plies, %ds) ---\n",
         STATIC_PLIES, TURN_TIME_LIMIT_S);
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);

  const Move *move = play_sim_turn(game, move_list, sim_results, &sim_ctx,
                                   win_pcts, tc, &STRATEGY_STATIC,
                                   STATIC_PLIES, error_stack);

  clock_gettime(CLOCK_MONOTONIC, &t1);
  double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

  string_builder_clear(sb);
  string_builder_add_move(sb, game_get_board(game), move,
                          game_get_ld(game), true);
  printf("\n  STATIC best: %s  [%.1fs]\n", string_builder_peek(sb), elapsed);

  // Reset game state for second run
  game_copy(game, game_copy_for_reset);

  // Run NESTED strategy
  printf("\n--- Running NESTED sim (%d plies, %ds) ---\n",
         NESTED_PLIES, TURN_TIME_LIMIT_S);
  clock_gettime(CLOCK_MONOTONIC, &t0);

  move = play_sim_turn(game, move_list, sim_results, &sim_ctx,
                       win_pcts, tc, &STRATEGY_NESTED,
                       NESTED_PLIES, error_stack);

  clock_gettime(CLOCK_MONOTONIC, &t1);
  elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

  string_builder_clear(sb);
  string_builder_add_move(sb, game_get_board(game), move,
                          game_get_ld(game), true);
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
  printf("  Late-game 99-ply sim, %ds/turn, %d threads\n",
         TURN_TIME_LIMIT_S, NUM_THREADS);
  printf("  Verifies cutoff=0 does not stop sim early\n");
  printf("================================================\n");

  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 15 -plies 2 -threads 10");
  load_and_exec_config_or_die(config, "cgp " PAIR23_TURN18_CGP);

  ErrorStack *load_es = error_stack_create();
  WinPct *win_pcts = win_pct_create(config_get_data_paths(config),
                                     DEFAULT_WIN_PCT, load_es);
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
  printf("\n--- Running STATIC sim (99 plies, %ds) ---\n",
         TURN_TIME_LIMIT_S);
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);

  const Move *move = play_sim_turn(game, move_list, sim_results, &sim_ctx,
                                   win_pcts, tc, &STRATEGY_STATIC,
                                   LATE_GAME_PLIES, error_stack);

  clock_gettime(CLOCK_MONOTONIC, &t1);
  double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

  string_builder_clear(sb);
  string_builder_add_move(sb, game_get_board(game), move,
                          game_get_ld(game), true);
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
