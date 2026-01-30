#include "benchmark_word_prune_test.h"

#include "../src/compat/ctime.h"
#include "../src/def/bai_defs.h"
#include "../src/def/equity_defs.h"
#include "../src/def/game_defs.h"
#include "../src/def/kwg_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/bai_result.h"
#include "../src/ent/dictionary_word.h"
#include "../src/ent/game.h"
#include "../src/ent/kwg.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/sim_args.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/wmp.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/kwg_maker.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/simmer.h"
#include "../src/impl/wmp_maker.h"
#include "../src/impl/word_prune.h"
#include "../src/str/game_string.h"
#include "../src/str/move_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_constants.h"
#include "test_util.h"
#include <assert.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define NUM_THREADS 16
#define NUM_PLIES 5
#define TARGET_TIME_PER_TURN_SEC 120.0  // 2 minutes per turn
#define NUM_GAMES 10
#define BASE_SEED 42

typedef struct TurnTimingData {
  int turn_number;
  int tiles_on_board;
  int tiles_in_bag;
  int num_moves_generated;

  // Word pruning overhead timings
  double word_prune_time_sec;
  double kwg_build_time_sec;
  double wmp_build_time_sec;
  double total_overhead_sec;
  int pruned_word_count;

  // Full WMP simulation timing
  double sim_time_sec;
  uint64_t num_sim_iterations;
  int full_word_count;

  // Actual movegen timing
  double movegen_full_time_us;    // avg movegen time with full KWG (microseconds)
  double movegen_pruned_time_us;  // avg movegen time with pruned KWG (microseconds)
  double movegen_speedup;         // actual speedup factor
  int movegen_iterations;         // number of iterations for timing

  // Derived metrics
  double word_reduction_pct;      // % reduction in words
  double estimated_movegen_speedup;  // estimated speedup factor for movegen
} TurnTimingData;

typedef struct GameTimingData {
  TurnTimingData turns[100];  // Max 100 turns per game
  int num_turns;
  uint64_t seed;
} GameTimingData;

// Execute config command quietly (suppress stdout during execution)
static void exec_config_quiet(Config *config, const char *cmd) {
  (void)fflush(stdout);
  int saved_stdout = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 0);
  int devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
  (void)dup2(devnull, STDOUT_FILENO);
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

  (void)fflush(stdout);
  (void)dup2(saved_stdout, STDOUT_FILENO);
  close(saved_stdout);
}

#define MOVEGEN_TIMING_ITERATIONS 100

// Measure actual movegen time with full and pruned KWG
static void measure_movegen_time(const Game *game, int num_threads,
                                 double *full_time_us, double *pruned_time_us,
                                 int *num_iterations, int *pruned_word_count) {
  Timer timer;
  MoveList *move_list = move_list_create(500);

  // First, generate the pruned word list and KWG
  DictionaryWordList *possible_words = dictionary_word_list_create();
  generate_possible_words(game, NULL, possible_words);
  *pruned_word_count = dictionary_word_list_get_count(possible_words);

  KWG *pruned_kwg = make_kwg_from_words_small(possible_words, KWG_MAKER_OUTPUT_DAWG,
                                               KWG_MAKER_MERGE_EXACT);
  dictionary_word_list_destroy(possible_words);

  // Time movegen with full KWG (using NULL for override_kwg)
  MoveGenArgs full_args = {
      .game = game,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      .thread_index = 0,
      .move_list = move_list,
  };

  // Warmup
  generate_moves(&full_args);

  ctimer_start(&timer);
  for (int i = 0; i < MOVEGEN_TIMING_ITERATIONS; i++) {
    move_list_reset(move_list);
    generate_moves(&full_args);
  }
  double full_total_time = ctimer_elapsed_seconds(&timer);
  *full_time_us = (full_total_time * 1000000.0) / MOVEGEN_TIMING_ITERATIONS;

  // Time movegen with pruned KWG
  MoveGenArgs pruned_args = {
      .game = game,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = pruned_kwg,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      .thread_index = 0,
      .move_list = move_list,
  };

  // Warmup
  move_list_reset(move_list);
  generate_moves(&pruned_args);

  ctimer_start(&timer);
  for (int i = 0; i < MOVEGEN_TIMING_ITERATIONS; i++) {
    move_list_reset(move_list);
    generate_moves(&pruned_args);
  }
  double pruned_total_time = ctimer_elapsed_seconds(&timer);
  *pruned_time_us = (pruned_total_time * 1000000.0) / MOVEGEN_TIMING_ITERATIONS;

  *num_iterations = MOVEGEN_TIMING_ITERATIONS;

  kwg_destroy(pruned_kwg);
  move_list_destroy(move_list);
}

static int count_tiles_on_board(const Game *game) {
  int count = 0;
  const Board *board = game_get_board(game);
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      if (board_get_letter(board, row, col) != ALPHABET_EMPTY_SQUARE_MARKER) {
        count++;
      }
    }
  }
  return count;
}

// Measure overhead of building pruned structures
static void measure_prune_overhead(const Game *game, int num_threads,
                                   double *word_prune_time,
                                   double *kwg_build_time,
                                   double *wmp_build_time,
                                   int *pruned_word_count) {
  Timer timer;

  // Step 1: Generate possible words (word pruning)
  ctimer_start(&timer);
  DictionaryWordList *possible_words = dictionary_word_list_create();
  generate_possible_words(game, NULL, possible_words);
  *word_prune_time = ctimer_elapsed_seconds(&timer);
  *pruned_word_count = dictionary_word_list_get_count(possible_words);

  // Step 2: Build KWG from pruned word list
  ctimer_start(&timer);
  KWG *pruned_kwg = make_kwg_from_words_small(possible_words, KWG_MAKER_OUTPUT_DAWG,
                                               KWG_MAKER_MERGE_EXACT);
  *kwg_build_time = ctimer_elapsed_seconds(&timer);

  // Step 3: Build WMP from pruned word list
  ctimer_start(&timer);
  WMP *pruned_wmp = make_wmp_from_words(possible_words, game_get_ld(game), num_threads);
  *wmp_build_time = ctimer_elapsed_seconds(&timer);

  dictionary_word_list_destroy(possible_words);
  kwg_destroy(pruned_kwg);
  wmp_destroy(pruned_wmp);
}

// Run simulation with full WMP
static double run_simulation(Config *config, int num_threads, int num_plies,
                             uint64_t seed, uint64_t *num_iterations) {
  Timer timer;
  ctimer_start(&timer);

  SimResults *sim_results = config_get_sim_results(config);
  SimCtx *sim_ctx = NULL;

  // Set up sim args
  char *set_cmd = get_formatted_string("set -threads %d -plies %d -seed %lu "
                                       "-iter 10000000 -scond 95 -sr tt -minp 50",
                                       num_threads, num_plies, seed);
  load_and_exec_config_or_die(config, set_cmd);
  free(set_cmd);

  // Generate moves
  load_and_exec_config_or_die(config, "gen");

  // Run simulation
  error_code_t status = config_simulate_and_return_status(config, &sim_ctx, NULL, sim_results);
  assert(status == ERROR_STATUS_SUCCESS);

  double sim_time = ctimer_elapsed_seconds(&timer);
  *num_iterations = sim_results_get_iteration_count(sim_results);

  sim_ctx_destroy(sim_ctx);

  return sim_time;
}

static void print_turn_data(const TurnTimingData *td) {
  printf("  Turn %2d: board=%3d tiles, bag=%2d tiles, moves=%4d\n",
         td->turn_number, td->tiles_on_board, td->tiles_in_bag,
         td->num_moves_generated);
  printf("    Movegen: full=%.0fus, pruned=%.0fus, speedup=%.2fx\n",
         td->movegen_full_time_us, td->movegen_pruned_time_us,
         td->movegen_speedup);
  printf("    Prune overhead: word_prune=%.3fs, kwg_build=%.3fs, "
         "wmp_build=%.3fs, total=%.3fs\n",
         td->word_prune_time_sec, td->kwg_build_time_sec,
         td->wmp_build_time_sec, td->total_overhead_sec);
  printf("    Simulation: time=%.3fs, iterations=%lu\n",
         td->sim_time_sec, td->num_sim_iterations);
  printf("    Words: pruned=%d (%.1f%% of full), overhead=%.1f%% of sim time\n",
         td->pruned_word_count, td->word_reduction_pct * 100.0,
         (td->total_overhead_sec / td->sim_time_sec) * 100.0);
}

static void run_single_game(Config *config, GameTimingData *game_data,
                            uint64_t seed, int full_word_count) {
  game_data->seed = seed;
  game_data->num_turns = 0;

  // Reset and seed the game
  exec_config_quiet(config, "new");
  Game *game = config_get_game(config);
  game_reset(game);
  game_seed(game, seed);
  draw_starting_racks(game);

  MoveList *move_list = move_list_create(100);
  int turn = 0;

  printf("\n--- Game seed %" PRIu64 " ---\n", seed);

  while (game_get_game_end_reason(game) == GAME_END_REASON_NONE) {
    TurnTimingData *td = &game_data->turns[game_data->num_turns];
    td->turn_number = turn + 1;
    td->tiles_on_board = count_tiles_on_board(game);
    td->tiles_in_bag = bag_get_letters(game_get_bag(game));
    td->full_word_count = full_word_count;

    // Skip very early game where pruning has minimal benefit
    // and very late game (endgame) which has different characteristics
    bool should_time = (td->tiles_on_board >= 10 && td->tiles_in_bag > 0);

    if (should_time) {
      // Measure actual movegen time with full and pruned KWG
      double full_time_us, pruned_time_us;
      int movegen_iters, pruned_word_count_mg;
      measure_movegen_time(game, NUM_THREADS, &full_time_us, &pruned_time_us,
                          &movegen_iters, &pruned_word_count_mg);

      td->movegen_full_time_us = full_time_us;
      td->movegen_pruned_time_us = pruned_time_us;
      td->movegen_speedup = full_time_us / pruned_time_us;
      td->movegen_iterations = movegen_iters;

      // Measure pruning overhead
      double word_prune_time, kwg_build_time, wmp_build_time;
      int pruned_word_count;
      measure_prune_overhead(game, NUM_THREADS, &word_prune_time,
                            &kwg_build_time, &wmp_build_time,
                            &pruned_word_count);

      td->word_prune_time_sec = word_prune_time;
      td->kwg_build_time_sec = kwg_build_time;
      td->wmp_build_time_sec = wmp_build_time;
      td->total_overhead_sec = word_prune_time + kwg_build_time + wmp_build_time;
      td->pruned_word_count = pruned_word_count;

      // Run simulation with full WMP
      uint64_t iterations;
      td->sim_time_sec = run_simulation(config, NUM_THREADS, NUM_PLIES,
                                        seed + (uint64_t)turn, &iterations);
      td->num_sim_iterations = iterations;

      // Get move count
      td->num_moves_generated = move_list_get_count(config_get_move_list(config));

      // Calculate derived metrics
      td->word_reduction_pct = (double)pruned_word_count / (double)full_word_count;
      td->estimated_movegen_speedup = (double)full_word_count / (double)pruned_word_count;

      print_turn_data(td);
      game_data->num_turns++;
    }

    // Play the best move to advance the game
    const Move *best_move = get_top_equity_move(game, 0, move_list);
    play_move(best_move, game, NULL);
    turn++;

    // Safety limit
    if (turn > 50) break;
  }

  move_list_destroy(move_list);
}

static void analyze_wmp_build_time(GameTimingData *games, int num_games) {
  printf("\n");
  printf("=======================================================\n");
  printf("            WMP BUILD TIME vs WORD COUNT\n");
  printf("=======================================================\n");
  printf("\n");

  // Collect all data points
  typedef struct {
    int word_count;
    double wmp_build_time;
  } DataPoint;

  DataPoint points[1000];
  int num_points = 0;

  for (int g = 0; g < num_games; g++) {
    for (int t = 0; t < games[g].num_turns && num_points < 1000; t++) {
      TurnTimingData *td = &games[g].turns[t];
      points[num_points].word_count = td->pruned_word_count;
      points[num_points].wmp_build_time = td->wmp_build_time_sec;
      num_points++;
    }
  }

  // Bucket by word count ranges (20K buckets)
  typedef struct {
    double total_wmp_time;
    int total_words;
    int count;
  } WordBucket;

  WordBucket word_buckets[15] = {0};  // 0-20K, 20-40K, ..., 280-300K

  for (int i = 0; i < num_points; i++) {
    int bucket_idx = points[i].word_count / 20000;
    if (bucket_idx >= 15) bucket_idx = 14;

    word_buckets[bucket_idx].total_wmp_time += points[i].wmp_build_time;
    word_buckets[bucket_idx].total_words += points[i].word_count;
    word_buckets[bucket_idx].count++;
  }

  printf("WMP build time by word count:\n");
  printf("%-15s | %-8s | %-12s | %-12s | %-12s\n",
         "Word Range", "Count", "Avg Words", "Avg Time(s)", "us/word");
  printf("----------------+----------+--------------+--------------+--------------\n");

  double total_us_per_word = 0;
  int buckets_with_data = 0;

  for (int i = 0; i < 15; i++) {
    if (word_buckets[i].count == 0) continue;

    double avg_words = (double)word_buckets[i].total_words / word_buckets[i].count;
    double avg_time = word_buckets[i].total_wmp_time / word_buckets[i].count;
    double us_per_word = (avg_time * 1000000.0) / avg_words;

    printf("%6dK-%6dK | %8d | %12.0f | %12.3f | %12.2f\n",
           i * 20, (i + 1) * 20, word_buckets[i].count,
           avg_words, avg_time, us_per_word);

    total_us_per_word += us_per_word;
    buckets_with_data++;
  }

  if (buckets_with_data > 0) {
    double avg_us_per_word = total_us_per_word / buckets_with_data;
    printf("\nAverage: %.2f microseconds per word\n", avg_us_per_word);
    printf("Estimated WMP build times:\n");
    printf("  10,000 words: %.0f ms\n", avg_us_per_word * 10000 / 1000);
    printf("  50,000 words: %.0f ms\n", avg_us_per_word * 50000 / 1000);
    printf("  100,000 words: %.0f ms\n", avg_us_per_word * 100000 / 1000);
    printf("  200,000 words: %.0f ms\n", avg_us_per_word * 200000 / 1000);
    printf("  280,000 words: %.0f ms\n", avg_us_per_word * 280000 / 1000);
  }
}

static void analyze_movegen_timing(GameTimingData *games, int num_games) {
  printf("\n");
  printf("=======================================================\n");
  printf("          ACTUAL MOVEGEN TIMING BY TILES ON BOARD\n");
  printf("=======================================================\n");
  printf("\n");

  typedef struct {
    double total_full_time;
    double total_pruned_time;
    double total_speedup;
    int count;
  } MovegenBucket;

  MovegenBucket buckets[15] = {0};  // 0-10, 10-20, ..., 130-140+ tiles

  for (int g = 0; g < num_games; g++) {
    for (int t = 0; t < games[g].num_turns; t++) {
      TurnTimingData *td = &games[g].turns[t];
      int bucket_idx = td->tiles_on_board / 10;
      if (bucket_idx >= 15) bucket_idx = 14;

      buckets[bucket_idx].total_full_time += td->movegen_full_time_us;
      buckets[bucket_idx].total_pruned_time += td->movegen_pruned_time_us;
      buckets[bucket_idx].total_speedup += td->movegen_speedup;
      buckets[bucket_idx].count++;
    }
  }

  printf("Movegen time by board fill level:\n");
  printf("%-12s | %-8s | %-12s | %-12s | %-10s\n",
         "Tiles", "Count", "Full (us)", "Pruned (us)", "Speedup");
  printf("-------------+----------+--------------+--------------+------------\n");

  for (int i = 0; i < 15; i++) {
    if (buckets[i].count == 0) continue;

    double avg_full = buckets[i].total_full_time / buckets[i].count;
    double avg_pruned = buckets[i].total_pruned_time / buckets[i].count;
    double avg_speedup = buckets[i].total_speedup / buckets[i].count;

    printf("%3d-%3d      | %8d | %12.0f | %12.0f | %10.2fx\n",
           i * 10, (i + 1) * 10 - 1, buckets[i].count,
           avg_full, avg_pruned, avg_speedup);
  }
}

static void analyze_results(GameTimingData *games, int num_games) {
  // First, analyze actual movegen timing
  analyze_movegen_timing(games, num_games);

  // Then analyze WMP build time vs word count
  analyze_wmp_build_time(games, num_games);

  printf("\n");
  printf("=======================================================\n");
  printf("             WORD PRUNING BENCHMARK ANALYSIS\n");
  printf("=======================================================\n");
  printf("\n");

  // Aggregate by tiles on board (bucket by 10s)
  typedef struct {
    double total_overhead;
    double total_sim_time;
    double total_word_reduction_pct;
    int count;
    int total_pruned_words;
    int total_full_words;
  } Bucket;

  Bucket buckets[15] = {0};  // 0-10, 10-20, ..., 130-140+ tiles

  for (int g = 0; g < num_games; g++) {
    for (int t = 0; t < games[g].num_turns; t++) {
      TurnTimingData *td = &games[g].turns[t];
      int bucket_idx = td->tiles_on_board / 10;
      if (bucket_idx >= 15) bucket_idx = 14;

      buckets[bucket_idx].total_overhead += td->total_overhead_sec;
      buckets[bucket_idx].total_sim_time += td->sim_time_sec;
      buckets[bucket_idx].total_word_reduction_pct += td->word_reduction_pct;
      buckets[bucket_idx].count++;
      buckets[bucket_idx].total_pruned_words += td->pruned_word_count;
      buckets[bucket_idx].total_full_words += td->full_word_count;
    }
  }

  printf("Results by board fill level:\n");
  printf("%-12s | %-8s | %-10s | %-10s | %-12s | %-10s | %-10s\n",
         "Tiles", "Count", "Overhead", "Sim Time", "Overhead %%", "Words %%", "Est Speedup");
  printf("-------------+----------+------------+------------+--------------+");
  printf("------------+------------\n");

  int break_even_bucket = -1;

  for (int i = 0; i < 15; i++) {
    if (buckets[i].count == 0) continue;

    double avg_overhead = buckets[i].total_overhead / buckets[i].count;
    double avg_sim_time = buckets[i].total_sim_time / buckets[i].count;
    double overhead_pct = (avg_overhead / avg_sim_time) * 100.0;
    double avg_word_pct = buckets[i].total_word_reduction_pct / buckets[i].count * 100.0;
    double avg_speedup = (double)buckets[i].total_full_words /
                         (double)buckets[i].total_pruned_words;

    // Estimate: movegen is ~80% of sim time. With pruned WMP, movegen speeds up
    // by avg_speedup factor. Net sim time = 0.2*sim + 0.8*sim/avg_speedup
    // For pruning to be beneficial: overhead < sim_time * (1 - (0.2 + 0.8/avg_speedup))
    double movegen_fraction = 0.8;
    double estimated_pruned_sim = avg_sim_time * (1.0 - movegen_fraction +
                                                   movegen_fraction / avg_speedup);
    double estimated_total_pruned = avg_overhead + estimated_pruned_sim;
    bool pruning_beneficial = estimated_total_pruned < avg_sim_time;

    printf("%3d-%3d      | %8d | %10.3fs | %10.3fs | %11.1f%% | %9.1f%% | %10.1fx\n",
           i * 10, (i + 1) * 10 - 1, buckets[i].count,
           avg_overhead, avg_sim_time, overhead_pct, avg_word_pct, avg_speedup);

    if (break_even_bucket < 0 && pruning_beneficial) {
      break_even_bucket = i;
    }
  }

  printf("\n");
  printf("=======================================================\n");
  printf("                    KEY FINDINGS\n");
  printf("=======================================================\n");
  printf("\nAssumptions:\n");
  printf("  - Movegen accounts for ~80%% of simulation time\n");
  printf("  - Movegen time scales linearly with WMP word count\n");
  printf("\n");

  printf("Analysis by board fill level:\n");
  for (int i = 0; i < 15; i++) {
    if (buckets[i].count == 0) continue;

    double avg_overhead = buckets[i].total_overhead / buckets[i].count;
    double avg_sim_time = buckets[i].total_sim_time / buckets[i].count;
    double avg_speedup = (double)buckets[i].total_full_words /
                         (double)buckets[i].total_pruned_words;

    double movegen_fraction = 0.8;
    double estimated_pruned_sim = avg_sim_time * (1.0 - movegen_fraction +
                                                   movegen_fraction / avg_speedup);
    double estimated_total_pruned = avg_overhead + estimated_pruned_sim;
    double savings = avg_sim_time - estimated_total_pruned;
    double savings_pct = (savings / avg_sim_time) * 100.0;

    if (savings > 0) {
      printf("  %3d-%3d tiles: PRUNE BENEFICIAL - estimated savings: %.1fs (%.1f%%)\n",
             i * 10, (i + 1) * 10 - 1, savings, savings_pct);
    } else {
      printf("  %3d-%3d tiles: FULL WMP BETTER - estimated overhead: %.1fs (%.1f%%)\n",
             i * 10, (i + 1) * 10 - 1, -savings, -savings_pct);
    }
  }

  if (break_even_bucket >= 0) {
    printf("\nBreak-even point: approximately %d tiles on board\n", break_even_bucket * 10);
    printf("Word pruning becomes beneficial when board has ~%d+ tiles.\n",
           break_even_bucket * 10);
  } else {
    printf("\nNo break-even point found in tested range.\n");
    printf("Word pruning overhead exceeds estimated benefits for all tested positions.\n");
  }

  printf("\nFor 2-minute per turn time control (%d plies, %d threads):\n",
         NUM_PLIES, NUM_THREADS);
  printf("  The break-even point indicates when you should start using word pruning.\n");
  printf("  Before that point, use the full WMP for better performance.\n");

  printf("\n");
}

void test_benchmark_word_prune(void) {
  log_set_level(LOG_WARN);

  printf("\n");
  printf("=======================================================\n");
  printf("  Word Pruning Benchmark for BAI Targeting\n");
  printf("  Config: %d threads, %d plies, CSW24\n", NUM_THREADS, NUM_PLIES);
  printf("  Target: 2 minutes per turn time control\n");
  printf("=======================================================\n");

  Config *config = config_create_or_die(
      "set -lex CSW24 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 15 -threads 16 -plies 5");

  // Get full word count from CSW24
  // CSW24 has approximately 281,000 words
  const int full_word_count = 281000;  // Approximate

  GameTimingData *games = calloc_or_die(NUM_GAMES, sizeof(GameTimingData));

  for (int g = 0; g < NUM_GAMES; g++) {
    run_single_game(config, &games[g], BASE_SEED + (uint64_t)g, full_word_count);
  }

  analyze_results(games, NUM_GAMES);

  free(games);
  config_destroy(config);
}
