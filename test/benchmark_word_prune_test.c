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
#define NUM_GAMES 10
#define BASE_SEED 42
#define MOVEGEN_TIMING_ITERATIONS 50

typedef struct TurnTimingData {
  int turn_number;
  int tiles_on_board;
  int tiles_in_bag;
  int num_moves_generated;
  int pruned_word_count;
  int full_word_count;

  // Word pruning overhead timings
  double word_prune_time_sec;
  double kwg_build_time_sec;
  double wmp_build_time_sec;
  double total_overhead_sec;

  // Simulation timing (with full KWG/WMP)
  double sim_full_time_sec;
  uint64_t sim_full_iterations;

  // Simulation timing (with pruned KWG/WMP for cross-sets + movegen)
  double sim_pruned_time_sec;
  uint64_t sim_pruned_iterations;

  // Total time comparison
  double total_full_time_sec;   // just sim time
  double total_pruned_time_sec; // overhead + pruned sim time

  // Actual movegen timing (single call)
  double movegen_full_time_us;
  double movegen_pruned_time_us;
  double movegen_speedup;
} TurnTimingData;

typedef struct GameTimingData {
  TurnTimingData turns[100];
  int num_turns;
  uint64_t seed;
} GameTimingData;

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

// Build pruned structures and return them
static void build_pruned_structures(const Game *game, int num_threads,
                                    DictionaryWordList **out_words,
                                    KWG **out_kwg, WMP **out_wmp,
                                    double *word_prune_time,
                                    double *kwg_build_time,
                                    double *wmp_build_time) {
  Timer timer;

  ctimer_start(&timer);
  *out_words = dictionary_word_list_create();
  generate_possible_words(game, NULL, *out_words);
  *word_prune_time = ctimer_elapsed_seconds(&timer);

  ctimer_start(&timer);
  *out_kwg = make_kwg_from_words_small(*out_words, KWG_MAKER_OUTPUT_DAWG,
                                        KWG_MAKER_MERGE_EXACT);
  *kwg_build_time = ctimer_elapsed_seconds(&timer);

  ctimer_start(&timer);
  *out_wmp = make_wmp_from_words(*out_words, game_get_ld(game), num_threads);
  *wmp_build_time = ctimer_elapsed_seconds(&timer);
}

// Run simulation and return timing info
static double run_sim_internal(Config *config, int num_threads, int num_plies,
                               uint64_t seed, uint64_t *out_iterations) {
  Timer timer;
  ctimer_start(&timer);

  SimResults *sim_results = config_get_sim_results(config);
  SimCtx *sim_ctx = NULL;

  char *set_cmd = get_formatted_string("set -threads %d -plies %d -seed %lu "
                                       "-iter 10000000 -scond 95 -sr tt -minp 50",
                                       num_threads, num_plies, seed);
  load_and_exec_config_or_die(config, set_cmd);
  free(set_cmd);

  load_and_exec_config_or_die(config, "gen");

  error_code_t status = config_simulate_and_return_status(config, &sim_ctx, NULL, sim_results);
  assert(status == ERROR_STATUS_SUCCESS);

  double sim_time = ctimer_elapsed_seconds(&timer);
  *out_iterations = sim_results_get_iteration_count(sim_results);

  sim_ctx_destroy(sim_ctx);
  return sim_time;
}

// Measure actual movegen time
static void measure_movegen_time(const Game *game, const KWG *pruned_kwg,
                                 double *full_time_us, double *pruned_time_us) {
  Timer timer;
  MoveList *move_list = move_list_create(500);

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

  // Warmup + time full
  generate_moves(&full_args);
  ctimer_start(&timer);
  for (int i = 0; i < MOVEGEN_TIMING_ITERATIONS; i++) {
    move_list_reset(move_list);
    generate_moves(&full_args);
  }
  *full_time_us = (ctimer_elapsed_seconds(&timer) * 1000000.0) / MOVEGEN_TIMING_ITERATIONS;

  MoveGenArgs pruned_args = full_args;
  pruned_args.override_kwg = pruned_kwg;

  // Warmup + time pruned
  move_list_reset(move_list);
  generate_moves(&pruned_args);
  ctimer_start(&timer);
  for (int i = 0; i < MOVEGEN_TIMING_ITERATIONS; i++) {
    move_list_reset(move_list);
    generate_moves(&pruned_args);
  }
  *pruned_time_us = (ctimer_elapsed_seconds(&timer) * 1000000.0) / MOVEGEN_TIMING_ITERATIONS;

  move_list_destroy(move_list);
}

static void print_turn_data(const TurnTimingData *td) {
  double speedup = td->total_full_time_sec / td->total_pruned_time_sec;
  printf("  Turn %2d: board=%3d tiles, bag=%2d\n",
         td->turn_number, td->tiles_on_board, td->tiles_in_bag);
  printf("    Words: %d pruned (%.1f%% of %d)\n",
         td->pruned_word_count,
         100.0 * td->pruned_word_count / td->full_word_count,
         td->full_word_count);
  printf("    Overhead: prune=%.3fs + kwg=%.3fs + wmp=%.3fs = %.3fs\n",
         td->word_prune_time_sec, td->kwg_build_time_sec,
         td->wmp_build_time_sec, td->total_overhead_sec);
  printf("    Sim full: %.3fs (%lu iters), Sim pruned: %.3fs (%lu iters)\n",
         td->sim_full_time_sec, td->sim_full_iterations,
         td->sim_pruned_time_sec, td->sim_pruned_iterations);
  printf("    Total: full=%.3fs, pruned=%.3fs -> %.2fx %s\n",
         td->total_full_time_sec, td->total_pruned_time_sec, speedup,
         speedup > 1.0 ? "FASTER" : "slower");
  printf("    Movegen: full=%.0fus, pruned=%.0fus (%.2fx)\n",
         td->movegen_full_time_us, td->movegen_pruned_time_us,
         td->movegen_speedup);
}

static void run_single_game(Config *config, GameTimingData *game_data,
                            uint64_t seed, int full_word_count) {
  game_data->seed = seed;
  game_data->num_turns = 0;

  load_and_exec_config_or_die(config, "new");
  Game *game = config_get_game(config);
  game_reset(game);
  game_seed(game, seed);
  draw_starting_racks(game);

  MoveList *move_list = move_list_create(100);
  int turn = 0;

  printf("\n--- Game seed %" PRIu64 " ---\n", seed);

  while (game_get_game_end_reason(game) == GAME_END_REASON_NONE) {
    int tiles_on_board = count_tiles_on_board(game);
    int tiles_in_bag = bag_get_letters(game_get_bag(game));

    // Only benchmark turns where we have meaningful board state
    bool should_time = (tiles_on_board >= 10 && tiles_in_bag > 0);

    if (should_time) {
      TurnTimingData *td = &game_data->turns[game_data->num_turns];
      td->turn_number = turn + 1;
      td->tiles_on_board = tiles_on_board;
      td->tiles_in_bag = tiles_in_bag;
      td->full_word_count = full_word_count;

      // Build pruned structures
      DictionaryWordList *pruned_words;
      KWG *pruned_kwg;
      WMP *pruned_wmp;
      build_pruned_structures(game, NUM_THREADS, &pruned_words, &pruned_kwg,
                              &pruned_wmp, &td->word_prune_time_sec,
                              &td->kwg_build_time_sec, &td->wmp_build_time_sec);
      td->total_overhead_sec = td->word_prune_time_sec + td->kwg_build_time_sec +
                               td->wmp_build_time_sec;
      td->pruned_word_count = dictionary_word_list_get_count(pruned_words);
      dictionary_word_list_destroy(pruned_words);

      // Measure movegen timing
      measure_movegen_time(game, pruned_kwg,
                          &td->movegen_full_time_us, &td->movegen_pruned_time_us);
      td->movegen_speedup = td->movegen_full_time_us / td->movegen_pruned_time_us;

      // Run simulation with FULL KWG/WMP
      td->sim_full_time_sec = run_sim_internal(config, NUM_THREADS, NUM_PLIES,
                                               seed + (uint64_t)turn,
                                               &td->sim_full_iterations);

      // Save original KWG/WMP for both players
      Player *p0 = game_get_player(game, 0);
      Player *p1 = game_get_player(game, 1);
      const KWG *orig_kwg0 = player_get_kwg(p0);
      const KWG *orig_kwg1 = player_get_kwg(p1);
      const WMP *orig_wmp0 = player_get_wmp(p0);
      const WMP *orig_wmp1 = player_get_wmp(p1);

      // Swap in pruned KWG/WMP for both players
      player_set_kwg(p0, pruned_kwg);
      player_set_kwg(p1, pruned_kwg);
      player_set_wmp(p0, pruned_wmp);
      player_set_wmp(p1, pruned_wmp);

      // Regenerate cross-sets with pruned KWG
      game_gen_all_cross_sets(game);

      // Run simulation with PRUNED KWG/WMP
      td->sim_pruned_time_sec = run_sim_internal(config, NUM_THREADS, NUM_PLIES,
                                                  seed + (uint64_t)turn,
                                                  &td->sim_pruned_iterations);

      // Restore original KWG/WMP
      player_set_kwg(p0, orig_kwg0);
      player_set_kwg(p1, orig_kwg1);
      player_set_wmp(p0, orig_wmp0);
      player_set_wmp(p1, orig_wmp1);

      // Regenerate cross-sets with original KWG
      game_gen_all_cross_sets(game);

      // Calculate totals
      td->total_full_time_sec = td->sim_full_time_sec;
      td->total_pruned_time_sec = td->total_overhead_sec + td->sim_pruned_time_sec;

      td->num_moves_generated = move_list_get_count(config_get_move_list(config));

      print_turn_data(td);
      game_data->num_turns++;

      kwg_destroy(pruned_kwg);
      wmp_destroy(pruned_wmp);
    }

    // Play best move to advance game
    const Move *best_move = get_top_equity_move(game, 0, move_list);
    play_move(best_move, game, NULL);
    turn++;

    if (turn > 50) break;
  }

  move_list_destroy(move_list);
}

static void analyze_results(GameTimingData *games, int num_games) {
  printf("\n");
  printf("=======================================================\n");
  printf("       WORD PRUNING BENCHMARK - FULL vs PRUNED SIM\n");
  printf("=======================================================\n");
  printf("\n");

  typedef struct {
    double total_full_time;
    double total_pruned_time;
    double total_overhead;
    double total_sim_full;
    double total_sim_pruned;
    double total_movegen_full;
    double total_movegen_pruned;
    int total_pruned_words;
    int total_full_words;
    int count;
  } Bucket;

  Bucket buckets[15] = {0};

  for (int g = 0; g < num_games; g++) {
    for (int t = 0; t < games[g].num_turns; t++) {
      TurnTimingData *td = &games[g].turns[t];
      int bucket_idx = td->tiles_on_board / 10;
      if (bucket_idx >= 15) bucket_idx = 14;

      buckets[bucket_idx].total_full_time += td->total_full_time_sec;
      buckets[bucket_idx].total_pruned_time += td->total_pruned_time_sec;
      buckets[bucket_idx].total_overhead += td->total_overhead_sec;
      buckets[bucket_idx].total_sim_full += td->sim_full_time_sec;
      buckets[bucket_idx].total_sim_pruned += td->sim_pruned_time_sec;
      buckets[bucket_idx].total_movegen_full += td->movegen_full_time_us;
      buckets[bucket_idx].total_movegen_pruned += td->movegen_pruned_time_us;
      buckets[bucket_idx].total_pruned_words += td->pruned_word_count;
      buckets[bucket_idx].total_full_words += td->full_word_count;
      buckets[bucket_idx].count++;
    }
  }

  printf("Results by board fill level:\n");
  printf("%-8s | %-5s | %-8s | %-8s | %-8s | %-8s | %-7s | %-7s\n",
         "Tiles", "N", "Full(s)", "Pruned(s)", "Overhead", "SimSpdup", "MG Full", "MG Prun");
  printf("---------+-------+----------+----------+----------+----------+---------+---------\n");

  int break_even_bucket = -1;

  for (int i = 0; i < 15; i++) {
    if (buckets[i].count == 0) continue;

    double avg_full = buckets[i].total_full_time / buckets[i].count;
    double avg_pruned = buckets[i].total_pruned_time / buckets[i].count;
    double avg_overhead = buckets[i].total_overhead / buckets[i].count;
    double avg_sim_full = buckets[i].total_sim_full / buckets[i].count;
    double avg_sim_pruned = buckets[i].total_sim_pruned / buckets[i].count;
    double sim_speedup = avg_sim_full / avg_sim_pruned;
    double avg_mg_full = buckets[i].total_movegen_full / buckets[i].count;
    double avg_mg_pruned = buckets[i].total_movegen_pruned / buckets[i].count;

    bool prune_wins = avg_pruned < avg_full;

    printf("%3d-%3d  | %5d | %8.3f | %8.3f | %8.3f | %8.2fx | %7.0f | %7.0f %s\n",
           i * 10, (i + 1) * 10 - 1, buckets[i].count,
           avg_full, avg_pruned, avg_overhead, sim_speedup,
           avg_mg_full, avg_mg_pruned,
           prune_wins ? "<- PRUNE WINS" : "");

    if (break_even_bucket < 0 && prune_wins) {
      break_even_bucket = i;
    }
  }

  printf("\n");
  printf("=======================================================\n");
  printf("                    KEY FINDINGS\n");
  printf("=======================================================\n");

  if (break_even_bucket >= 0) {
    printf("\nBreak-even point: ~%d tiles on board\n", break_even_bucket * 10);
    printf("Word pruning becomes beneficial when board has ~%d+ tiles.\n",
           break_even_bucket * 10);
  } else {
    printf("\nNo break-even point found - pruning not beneficial in tested range.\n");
  }

  printf("\nFor ~2 minute per turn time control (%d plies, %d threads):\n",
         NUM_PLIES, NUM_THREADS);
  printf("  Pruning includes: word_prune + KWG build + WMP build\n");
  printf("  Pruned KWG is used for both movegen AND cross-set computation\n");
  printf("\n");
}

void test_benchmark_word_prune(void) {
  log_set_level(LOG_WARN);

  printf("\n");
  printf("=======================================================\n");
  printf("  Word Pruning Benchmark for BAI Targeting\n");
  printf("  Config: %d threads, %d plies, CSW24\n", NUM_THREADS, NUM_PLIES);
  printf("  Pruned KWG used for cross-sets AND movegen\n");
  printf("=======================================================\n");

  Config *config = config_create_or_die(
      "set -lex CSW24 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 15 -threads 16 -plies 5");

  const int full_word_count = 281000;

  GameTimingData *games = calloc_or_die(NUM_GAMES, sizeof(GameTimingData));

  for (int g = 0; g < NUM_GAMES; g++) {
    run_single_game(config, &games[g], BASE_SEED + (uint64_t)g, full_word_count);
  }

  analyze_results(games, NUM_GAMES);

  free(games);
  config_destroy(config);
}
