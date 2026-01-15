#include "eval_endgame_test.h"

#include "../src/def/game_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/board.h"
#include "../src/ent/endgame_results.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/thread_control.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/endgame.h"
#include "../src/impl/exec.h"
#include "../src/impl/gameplay.h"
#include "../src/str/game_string.h"
#include "../src/str/move_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// Execute config command quietly (suppress stdout during execution)
static void exec_config_quiet(Config *config, const char *cmd) {
  fflush(stdout);
  int saved_stdout = dup(STDOUT_FILENO);
  int devnull = open("/dev/null", O_WRONLY);
  dup2(devnull, STDOUT_FILENO);
  close(devnull);

  ErrorStack *error_stack = error_stack_create();
  thread_control_set_status(config_get_thread_control(config),
                            THREAD_CONTROL_STATUS_STARTED);
  config_load_command(config, cmd, error_stack);
  assert(error_stack_is_empty(error_stack));
  config_execute_command(config, error_stack);
  assert(error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);
  thread_control_set_status(config_get_thread_control(config),
                            THREAD_CONTROL_STATUS_FINISHED);

  fflush(stdout);
  dup2(saved_stdout, STDOUT_FILENO);
  close(saved_stdout);
}

static double get_time_sec(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// Player A configuration
// Set PLAYER_A_PLIES to 0 for static evaluation, or >0 for endgame solver
static const int PLAYER_A_PLIES = 5;
static const bool PLAYER_A_FIRST_WIN_OPTIM = true;
static const char *PLAYER_A_NAME = "Endgame5Win";

// Player B configuration
static const int PLAYER_B_PLIES = 6;
static const bool PLAYER_B_FIRST_WIN_OPTIM = true;
static const char *PLAYER_B_NAME = "Endgame6Win";

typedef enum {
  STRATEGY_A,  // Player A strategy
  STRATEGY_B   // Player B strategy
} EndgameStrategy;

typedef struct {
  int strat_a_wins;
  int strat_b_wins;
  int ties;
  int pairs_split;          // Each strategy won one game
  int pairs_swept_a;        // Strategy A won both
  int pairs_swept_b;        // Strategy B won both
  int pairs_both_tied;      // Both games tied
  int pairs_one_tie_a_leads; // One tie, Static won the other (Static 1.5, Endgame 0.5)
  int pairs_one_tie_b_leads; // One tie, Endgame won the other (Static 0.5, Endgame 1.5)
  int total_spread_a;       // Total spread for Static across all games
  int total_spread_b;       // Total spread for Endgame across all games
  int pairs_spread_a_better; // Pairs where Static had better combined spread
  int pairs_spread_b_better; // Pairs where Endgame had better combined spread
  int pairs_spread_tied;     // Pairs where combined spread was equal
} StrategyCompareStats;

// Timing stats accumulator
typedef struct {
  double *times;
  int count;
  int capacity;
  double total;
} TimingStats;

static void timing_stats_init(TimingStats *ts, int capacity) {
  ts->times = malloc(sizeof(double) * capacity);
  ts->count = 0;
  ts->capacity = capacity;
  ts->total = 0;
}

static void timing_stats_add(TimingStats *ts, double time) {
  if (ts->count < ts->capacity) {
    ts->times[ts->count++] = time;
    ts->total += time;
  }
}

static int compare_doubles(const void *a, const void *b) {
  double da = *(const double *)a;
  double db = *(const double *)b;
  return (da > db) - (da < db);
}

static void timing_stats_print(TimingStats *ts, const char *name) {
  if (ts->count == 0) {
    printf("  %s: no data\n", name);
    return;
  }

  // Sort for percentiles
  qsort(ts->times, ts->count, sizeof(double), compare_doubles);

  double mean = ts->total / ts->count;
  double median = ts->times[ts->count / 2];
  double max = ts->times[ts->count - 1];
  double min = ts->times[0];

  printf("  %s timing (%d samples):\n", name, ts->count);
  printf("    Total:  %.1f ms\n", ts->total * 1000);
  printf("    Mean:   %.3f ms\n", mean * 1000);
  printf("    Median: %.3f ms\n", median * 1000);
  printf("    Min:    %.3f ms, Max: %.3f ms\n", min * 1000, max * 1000);

  // Create histogram with log-scale buckets for better visualization of long-tail data
  // Buckets: <1ms, 1-10ms, 10-100ms, 100ms-1s, 1s-10s, >10s
  const int num_buckets = 6;
  double bucket_bounds[] = {0, 0.001, 0.01, 0.1, 1.0, 10.0, 1e9};
  const char *bucket_labels[] = {"<1ms", "1-10ms", "10-100ms", "100ms-1s", "1s-10s", ">10s"};
  int bucket_counts[6] = {0};

  for (int i = 0; i < ts->count; i++) {
    for (int b = 0; b < num_buckets; b++) {
      if (ts->times[i] >= bucket_bounds[b] && ts->times[i] < bucket_bounds[b + 1]) {
        bucket_counts[b]++;
        break;
      }
    }
  }

  // Find max count for scaling
  int max_count = 0;
  for (int b = 0; b < num_buckets; b++) {
    if (bucket_counts[b] > max_count) max_count = bucket_counts[b];
  }

  printf("    Distribution:\n");
  const int max_bar = 40;
  for (int b = 0; b < num_buckets; b++) {
    if (bucket_counts[b] == 0) continue;
    int bar_len = (max_count > 0) ? (bucket_counts[b] * max_bar / max_count) : 0;
    if (bar_len == 0 && bucket_counts[b] > 0) bar_len = 1;
    printf("    %9s: %4d (%5.1f%%) |", bucket_labels[b], bucket_counts[b],
           100.0 * bucket_counts[b] / ts->count);
    for (int j = 0; j < bar_len; j++) {
      printf("#");
    }
    printf("\n");
  }
}

static void timing_stats_destroy(TimingStats *ts) {
  free(ts->times);
}

// Simple dynamic string list for collecting CGP strings
typedef struct {
  char **strings;
  int count;
  int capacity;
} CGPList;

static void cgp_list_init(CGPList *sl, int capacity) {
  sl->strings = malloc(sizeof(char *) * capacity);
  sl->count = 0;
  sl->capacity = capacity;
}

static void cgp_list_add(CGPList *sl, const char *str) {
  if (sl->count >= sl->capacity) {
    sl->capacity *= 2;
    sl->strings = realloc(sl->strings, sizeof(char *) * sl->capacity);
  }
  sl->strings[sl->count++] = strdup(str);
}

static void cgp_list_destroy(CGPList *sl) {
  for (int i = 0; i < sl->count; i++) {
    free(sl->strings[i]);
  }
  free(sl->strings);
}

// Get move using static evaluation (top equity)
static double get_static_move(Game *game, MoveList *move_list, Move *out_move) {
  double start = get_time_sec();
  Move *top = get_top_equity_move(game, 0, move_list);
  move_copy(out_move, top);
  return get_time_sec() - start;
}

// Get move using endgame solver
static double get_endgame_move(Game *game, EndgameSolver *solver, Config *config,
                               int plies, bool first_win_optim, Move *out_move) {
  double start = get_time_sec();

  EndgameArgs args = {.game = game,
                      .thread_control = config_get_thread_control(config),
                      .plies = plies,
                      .spread_plies = 0,
                      .tt_fraction_of_mem = 0.5,
                      .initial_small_move_arena_size =
                          DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
                      .num_threads = 10,
                      .per_ply_callback = NULL,
                      .per_ply_callback_data = NULL,
                      .first_win_optim = first_win_optim,
                      .first_win_then_spread = false};
  EndgameResults *results = config_get_endgame_results(config);
  ErrorStack *err = error_stack_create();

  endgame_solve(solver, &args, results, err);
  assert(error_stack_is_empty(err));
  error_stack_destroy(err);

  // Get the best move from results
  const PVLine *pv = endgame_results_get_pvline(results);
  if (pv->num_moves > 0) {
    small_move_to_move(out_move, &pv->moves[0], game_get_board(game));
  } else {
    // Fallback to pass if no PV
    move_set_as_pass(out_move);
  }

  return get_time_sec() - start;
}

// Play out an endgame with given strategies for each player
// Returns final spread from player 0's perspective
// move_log receives the sequence of moves played
// game_static_time and game_endgame_time receive total time per game for each strategy
static int play_endgame_with_strategies(Game *game, EndgameStrategy strat_p0,
                                        EndgameStrategy strat_p1,
                                        EndgameSolver *solver, Config *config,
                                        MoveList *move_list,
                                        TimingStats *static_turn_times,
                                        TimingStats *endgame_turn_times,
                                        StringBuilder *move_log,
                                        double *game_static_time,
                                        double *game_endgame_time) {
  Move move;
  const LetterDistribution *ld = game_get_ld(game);
  int move_num = 0;
  *game_static_time = 0;
  *game_endgame_time = 0;

  while (game_get_game_end_reason(game) == GAME_END_REASON_NONE) {
    int player_on_turn = game_get_player_on_turn_index(game);
    EndgameStrategy strat = (player_on_turn == 0) ? strat_p0 : strat_p1;
    double elapsed;

    if (strat == STRATEGY_A) {
      if (PLAYER_A_PLIES == 0) {
        elapsed = get_static_move(game, move_list, &move);
      } else {
        elapsed = get_endgame_move(game, solver, config, PLAYER_A_PLIES,
                                   PLAYER_A_FIRST_WIN_OPTIM, &move);
      }
      timing_stats_add(static_turn_times, elapsed);
      *game_static_time += elapsed;
    } else {
      if (PLAYER_B_PLIES == 0) {
        elapsed = get_static_move(game, move_list, &move);
      } else {
        elapsed = get_endgame_move(game, solver, config, PLAYER_B_PLIES,
                                   PLAYER_B_FIRST_WIN_OPTIM, &move);
      }
      timing_stats_add(endgame_turn_times, elapsed);
      *game_endgame_time += elapsed;
    }

    // Log the move in human-readable format: "8A QUEY 35"
    if (move_num > 0) {
      string_builder_add_string(move_log, "; ");
    }
    string_builder_add_move(move_log, game_get_board(game), &move, ld, false);
    string_builder_add_formatted_string(move_log, " %d",
                                        equity_to_int(move_get_score(&move)));
    move_num++;

    play_move(&move, game, NULL);
  }

  // Return final spread from player 0's perspective
  int score0 = equity_to_int(player_get_score(game_get_player(game, 0)));
  int score1 = equity_to_int(player_get_score(game_get_player(game, 1)));
  return score0 - score1;
}

// Play until bag is empty, return true if valid endgame position
static bool play_until_bag_empty(Game *game, MoveList *move_list) {
  while (bag_get_letters(game_get_bag(game)) > 0) {
    Move *move = get_top_equity_move(game, 0, move_list);
    play_move(move, game, NULL);

    if (game_get_game_end_reason(game) != GAME_END_REASON_NONE) {
      return false;
    }
  }

  const Rack *rack0 = player_get_rack(game_get_player(game, 0));
  const Rack *rack1 = player_get_rack(game_get_player(game, 1));
  return !rack_is_empty(rack0) && !rack_is_empty(rack1);
}

void test_eval_endgame(void) {
  log_set_level(LOG_FATAL);  // Suppress all but fatal errors

  const int num_positions = 250;
  const uint64_t base_seed = 0;

  // Estimate max moves: ~10 moves per endgame * 2 games * 1000 positions
  const int max_timing_entries = 50000;

  Config *config = config_create_or_die(
      "set -lex CSW21 -threads 1 -s1 score -s2 score -r1 small -r2 small");

  EndgameSolver *solver = endgame_solver_create();
  MoveList *move_list = move_list_create(1);

  // Create the initial game
  exec_config_quiet(config, "new");
  Game *game = config_get_game(config);

  StrategyCompareStats stats = {0};
  TimingStats static_turn_times, endgame_turn_times;
  TimingStats static_game_times, endgame_game_times;
  timing_stats_init(&static_turn_times, max_timing_entries);
  timing_stats_init(&endgame_turn_times, max_timing_entries);
  timing_stats_init(&static_game_times, num_positions * 2);
  timing_stats_init(&endgame_game_times, num_positions * 2);

  // CGP lists for non-split outcomes
  CGPList cgp_swept_a, cgp_swept_b, cgp_both_tied;
  CGPList cgp_one_tie_a_leads, cgp_one_tie_b_leads;
  cgp_list_init(&cgp_swept_a, 16);
  cgp_list_init(&cgp_swept_b, 16);
  cgp_list_init(&cgp_both_tied, 16);
  cgp_list_init(&cgp_one_tie_a_leads, 16);
  cgp_list_init(&cgp_one_tie_b_leads, 16);

  printf("\n");
  printf("==============================================\n");
  printf("  Strategy Comparison: %s vs %s\n", PLAYER_A_NAME, PLAYER_B_NAME);
  if (PLAYER_A_PLIES == 0) {
    printf("  %s: static evaluation\n", PLAYER_A_NAME);
  } else {
    printf("  %s: %d-ply, first_win_optim=%s\n", PLAYER_A_NAME, PLAYER_A_PLIES,
           PLAYER_A_FIRST_WIN_OPTIM ? "true" : "false");
  }
  if (PLAYER_B_PLIES == 0) {
    printf("  %s: static evaluation\n", PLAYER_B_NAME);
  } else {
    printf("  %s: %d-ply, first_win_optim=%s\n", PLAYER_B_NAME, PLAYER_B_PLIES,
           PLAYER_B_FIRST_WIN_OPTIM ? "true" : "false");
  }
  printf("  Positions: %d (x2 games each = %d total games)\n",
         num_positions, num_positions * 2);
  printf("==============================================\n\n");

  int valid_positions = 0;
  for (int i = 0; valid_positions < num_positions; i++) {
    // Generate endgame position
    game_reset(game);
    game_seed(game, base_seed + (uint64_t)i);
    draw_starting_racks(game);

    if (!play_until_bag_empty(game, move_list)) {
      continue;
    }

    // Save the position for replay
    Game *saved_game = game_duplicate(game);

    // Print position header
    printf("\n--- Pair %d (seed %llu) ---\n", valid_positions + 1,
           (unsigned long long)(base_seed + (uint64_t)i));
    StringBuilder *game_sb = string_builder_create();
    GameStringOptions *gso = game_string_options_create_default();
    string_builder_add_game(saved_game, NULL, gso, NULL, game_sb);
    printf("%s", string_builder_peek(game_sb));
    string_builder_destroy(game_sb);
    game_string_options_destroy(gso);

    // Game 1: Static (P0) vs Endgame (P1)
    Game *game1 = game_duplicate(saved_game);
    StringBuilder *moves1 = string_builder_create();
    double game1_static_time, game1_endgame_time;
    int spread1 = play_endgame_with_strategies(game1, STRATEGY_A,
                                                STRATEGY_B,
                                                solver, config, move_list,
                                                &static_turn_times, &endgame_turn_times,
                                                moves1,
                                                &game1_static_time, &game1_endgame_time);
    timing_stats_add(&static_game_times, game1_static_time);
    timing_stats_add(&endgame_game_times, game1_endgame_time);
    game_destroy(game1);

    // Game 2: Endgame (P0) vs Static (P1)
    Game *game2 = game_duplicate(saved_game);
    StringBuilder *moves2 = string_builder_create();
    double game2_static_time, game2_endgame_time;
    int spread2 = play_endgame_with_strategies(game2, STRATEGY_B,
                                                STRATEGY_A,
                                                solver, config, move_list,
                                                &static_turn_times, &endgame_turn_times,
                                                moves2,
                                                &game2_static_time, &game2_endgame_time);
    timing_stats_add(&static_game_times, game2_static_time);
    timing_stats_add(&endgame_game_times, game2_endgame_time);
    game_destroy(game2);

    // Print game results with move sequences
    const char *winner1 = (spread1 > 0) ? PLAYER_A_NAME :
                          (spread1 < 0) ? PLAYER_B_NAME : "Tie";
    const char *winner2 = (spread2 > 0) ? PLAYER_B_NAME :
                          (spread2 < 0) ? PLAYER_A_NAME : "Tie";
    printf("  Game %d.1 (%s vs %s): %s by %d: %s\n",
           valid_positions + 1, PLAYER_A_NAME, PLAYER_B_NAME,
           winner1, spread1 > 0 ? spread1 : -spread1,
           string_builder_peek(moves1));
    printf("  Game %d.2 (%s vs %s): %s by %d: %s\n",
           valid_positions + 1, PLAYER_B_NAME, PLAYER_A_NAME,
           winner2, spread2 > 0 ? spread2 : -spread2,
           string_builder_peek(moves2));
    printf("  Time: %s %.1fms + %.1fms = %.1fms, %s %.1fms + %.1fms = %.1fms\n",
           PLAYER_A_NAME, game1_static_time * 1000, game2_static_time * 1000,
           (game1_static_time + game2_static_time) * 1000,
           PLAYER_B_NAME, game1_endgame_time * 1000, game2_endgame_time * 1000,
           (game1_endgame_time + game2_endgame_time) * 1000);

    string_builder_destroy(moves1);
    string_builder_destroy(moves2);

    // Analyze results
    // In game1: Static is P0, Endgame is P1
    //   spread1 > 0 means Static won, spread1 < 0 means Endgame won
    // In game2: Endgame is P0, Static is P1
    //   spread2 > 0 means Endgame won, spread2 < 0 means Static won

    int static_result1 = (spread1 > 0) ? 1 : (spread1 < 0) ? -1 : 0;
    int endgame_result1 = -static_result1;

    int endgame_result2 = (spread2 > 0) ? 1 : (spread2 < 0) ? -1 : 0;
    int static_result2 = -endgame_result2;

    // Update win counts
    if (static_result1 > 0) stats.strat_a_wins++;
    else if (static_result1 < 0) stats.strat_b_wins++;
    else stats.ties++;

    if (static_result2 > 0) stats.strat_a_wins++;
    else if (static_result2 < 0) stats.strat_b_wins++;
    else stats.ties++;

    // Track spreads
    // Game1: Static is P0, so spread1 is Static's spread
    // Game2: Endgame is P0, so -spread2 is Static's spread
    int static_spread_game1 = spread1;
    int static_spread_game2 = -spread2;
    int static_pair_spread = static_spread_game1 + static_spread_game2;
    int endgame_pair_spread = -static_pair_spread;

    stats.total_spread_a += static_spread_game1 + static_spread_game2;
    stats.total_spread_b += (-spread1) + spread2;  // Endgame's spreads

    if (static_pair_spread > endgame_pair_spread) {
      stats.pairs_spread_a_better++;
    } else if (endgame_pair_spread > static_pair_spread) {
      stats.pairs_spread_b_better++;
    } else {
      stats.pairs_spread_tied++;
    }

    // Analyze pair outcome and collect CGP for non-split outcomes
    int static_pair = static_result1 + static_result2;
    int endgame_pair = endgame_result1 + endgame_result2;

    // Generate CGP for potential collection (only for non-split outcomes)
    char *cgp = NULL;

    if (static_result1 == 0 && static_result2 == 0) {
      stats.pairs_both_tied++;
      cgp = game_get_cgp(saved_game, true);
      cgp_list_add(&cgp_both_tied, cgp);
    } else if (static_result1 == 0 || static_result2 == 0) {
      // One tie - who won the other game?
      cgp = game_get_cgp(saved_game, true);
      if (static_pair > 0) {
        stats.pairs_one_tie_a_leads++;  // Static 1.5, Endgame 0.5
        cgp_list_add(&cgp_one_tie_a_leads, cgp);
      } else {
        stats.pairs_one_tie_b_leads++;  // Static 0.5, Endgame 1.5
        cgp_list_add(&cgp_one_tie_b_leads, cgp);
      }
    } else if (static_pair > 0) {
      stats.pairs_swept_a++;  // Static swept
      cgp = game_get_cgp(saved_game, true);
      cgp_list_add(&cgp_swept_a, cgp);
    } else if (endgame_pair > 0) {
      stats.pairs_swept_b++;  // Endgame swept
      cgp = game_get_cgp(saved_game, true);
      cgp_list_add(&cgp_swept_b, cgp);
    } else {
      stats.pairs_split++;
      // No CGP collection for split outcomes
    }

    if (cgp) {
      free(cgp);
    }

    game_destroy(saved_game);
    valid_positions++;
  }

  printf("\n");
  printf("==============================================\n");
  printf("  RESULTS\n");
  printf("==============================================\n");
  printf("  Total games: %d\n", num_positions * 2);
  printf("  %s wins: %d (%.1f%%)\n", PLAYER_A_NAME,
         stats.strat_a_wins, 100.0 * stats.strat_a_wins / (num_positions * 2));
  printf("  %s wins: %d (%.1f%%)\n", PLAYER_B_NAME,
         stats.strat_b_wins, 100.0 * stats.strat_b_wins / (num_positions * 2));
  printf("  Ties: %d (%.1f%%)\n",
         stats.ties, 100.0 * stats.ties / (num_positions * 2));
  printf("\n");
  printf("  Spread:\n");
  printf("    %s total: %+d (avg %+.3f)\n", PLAYER_A_NAME,
         stats.total_spread_a, (double)stats.total_spread_a / (num_positions * 2));
  printf("    %s total: %+d (avg %+.3f)\n", PLAYER_B_NAME,
         stats.total_spread_b, (double)stats.total_spread_b / (num_positions * 2));
  printf("\n");
  printf("  Pair analysis (%d pairs):\n", num_positions);
  printf("    %s swept: %d (%.1f%%)\n", PLAYER_A_NAME,
         stats.pairs_swept_a, 100.0 * stats.pairs_swept_a / num_positions);
  printf("    %s swept: %d (%.1f%%)\n", PLAYER_B_NAME,
         stats.pairs_swept_b, 100.0 * stats.pairs_swept_b / num_positions);
  printf("    Split: %d (%.1f%%)\n",
         stats.pairs_split, 100.0 * stats.pairs_split / num_positions);
  printf("    Both tied: %d (%.1f%%)\n",
         stats.pairs_both_tied, 100.0 * stats.pairs_both_tied / num_positions);
  printf("    One tie (%s 1.5): %d (%.1f%%)\n", PLAYER_A_NAME,
         stats.pairs_one_tie_a_leads, 100.0 * stats.pairs_one_tie_a_leads / num_positions);
  printf("    One tie (%s 1.5): %d (%.1f%%)\n", PLAYER_B_NAME,
         stats.pairs_one_tie_b_leads, 100.0 * stats.pairs_one_tie_b_leads / num_positions);
  printf("  Pair spread:\n");
  printf("    %s better: %d (%.1f%%)\n", PLAYER_A_NAME,
         stats.pairs_spread_a_better, 100.0 * stats.pairs_spread_a_better / num_positions);
  printf("    %s better: %d (%.1f%%)\n", PLAYER_B_NAME,
         stats.pairs_spread_b_better, 100.0 * stats.pairs_spread_b_better / num_positions);
  printf("    Equal: %d (%.1f%%)\n",
         stats.pairs_spread_tied, 100.0 * stats.pairs_spread_tied / num_positions);
  printf("\n");
  printf("  PER-TURN TIMING:\n");

  char timing_label_a[64], timing_label_b[64];
  snprintf(timing_label_a, sizeof(timing_label_a), "%s (per turn)", PLAYER_A_NAME);
  snprintf(timing_label_b, sizeof(timing_label_b), "%s (per turn)", PLAYER_B_NAME);
  timing_stats_print(&static_turn_times, timing_label_a);
  timing_stats_print(&endgame_turn_times, timing_label_b);
  printf("\n");
  printf("  PER-GAME TIMING:\n");
  snprintf(timing_label_a, sizeof(timing_label_a), "%s (per game)", PLAYER_A_NAME);
  snprintf(timing_label_b, sizeof(timing_label_b), "%s (per game)", PLAYER_B_NAME);
  timing_stats_print(&static_game_times, timing_label_a);
  timing_stats_print(&endgame_game_times, timing_label_b);
  printf("==============================================\n\n");

  // Print CGP lists for non-split outcomes
  int total_nonsplit = cgp_swept_a.count + cgp_swept_b.count +
                       cgp_both_tied.count + cgp_one_tie_a_leads.count +
                       cgp_one_tie_b_leads.count;
  if (total_nonsplit > 0) {
    printf("==============================================\n");
    printf("  NON-SPLIT CGP POSITIONS (%d total)\n", total_nonsplit);
    printf("==============================================\n");

    if (cgp_swept_a.count > 0) {
      printf("\n  %s swept (%d):\n", PLAYER_A_NAME, cgp_swept_a.count);
      for (int i = 0; i < cgp_swept_a.count; i++) {
        printf("    %s\n", cgp_swept_a.strings[i]);
      }
    }

    if (cgp_swept_b.count > 0) {
      printf("\n  %s swept (%d):\n", PLAYER_B_NAME, cgp_swept_b.count);
      for (int i = 0; i < cgp_swept_b.count; i++) {
        printf("    %s\n", cgp_swept_b.strings[i]);
      }
    }

    if (cgp_both_tied.count > 0) {
      printf("\n  Both games tied (%d):\n", cgp_both_tied.count);
      for (int i = 0; i < cgp_both_tied.count; i++) {
        printf("    %s\n", cgp_both_tied.strings[i]);
      }
    }

    if (cgp_one_tie_a_leads.count > 0) {
      printf("\n  One tie, %s leads (%d):\n", PLAYER_A_NAME, cgp_one_tie_a_leads.count);
      for (int i = 0; i < cgp_one_tie_a_leads.count; i++) {
        printf("    %s\n", cgp_one_tie_a_leads.strings[i]);
      }
    }

    if (cgp_one_tie_b_leads.count > 0) {
      printf("\n  One tie, %s leads (%d):\n", PLAYER_B_NAME, cgp_one_tie_b_leads.count);
      for (int i = 0; i < cgp_one_tie_b_leads.count; i++) {
        printf("    %s\n", cgp_one_tie_b_leads.strings[i]);
      }
    }

    printf("\n==============================================\n\n");
  }

  cgp_list_destroy(&cgp_swept_a);
  cgp_list_destroy(&cgp_swept_b);
  cgp_list_destroy(&cgp_both_tied);
  cgp_list_destroy(&cgp_one_tie_a_leads);
  cgp_list_destroy(&cgp_one_tie_b_leads);

  timing_stats_destroy(&static_turn_times);
  timing_stats_destroy(&endgame_turn_times);
  timing_stats_destroy(&static_game_times);
  timing_stats_destroy(&endgame_game_times);
  move_list_destroy(move_list);
  endgame_solver_destroy(solver);
  config_destroy(config);
}
