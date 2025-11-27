#include "inference_comparison_test.h"

#include "../src/ent/alias_method.h"
#include "../src/ent/bag.h"
#include "../src/ent/encoded_rack.h"
#include "../src/ent/game.h"
#include "../src/ent/inference_results.h"
#include "../src/ent/move.h"
#include "../src/ent/rack.h"
#include "../src/ent/thread_control.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/inference.h"
#include "../src/impl/move_gen.h"
#include "../src/str/move_string.h"
#include "../src/str/rack_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_constants.h"
#include "test_util.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Structure to hold a rack+count pair for sorting
typedef struct {
  EncodedRack rack;
  uint32_t count;
} RackCountPair;

// Compare function for qsort - compare encoded racks
static int compare_rack_count_pairs(const void *a, const void *b) {
  const RackCountPair *pa = (const RackCountPair *)a;
  const RackCountPair *pb = (const RackCountPair *)b;
  for (int i = 0; i < ENCODED_RACK_UNITS; i++) {
    if (pa->rack.array[i] < pb->rack.array[i]) return -1;
    if (pa->rack.array[i] > pb->rack.array[i]) return 1;
  }
  return 0;
}

// Compare two AliasMethod structs for equality
// Returns true if equal, false otherwise
// If verbose and not equal, prints the differences
static bool compare_alias_methods(const AliasMethod *am_legacy,
                                  const AliasMethod *am_new,
                                  const LetterDistribution *ld,
                                  bool verbose) {
  uint32_t n_legacy = am_legacy->num_items;
  uint32_t n_new = am_new->num_items;

  if (n_legacy == 0 && n_new == 0) {
    return true;
  }

  RackCountPair *legacy_pairs = malloc(n_legacy * sizeof(RackCountPair));
  RackCountPair *new_pairs = malloc(n_new * sizeof(RackCountPair));

  for (uint32_t i = 0; i < n_legacy; i++) {
    legacy_pairs[i].rack = am_legacy->items[i].rack;
    legacy_pairs[i].count = am_legacy->items[i].count;
  }
  for (uint32_t i = 0; i < n_new; i++) {
    new_pairs[i].rack = am_new->items[i].rack;
    new_pairs[i].count = am_new->items[i].count;
  }

  qsort(legacy_pairs, n_legacy, sizeof(RackCountPair), compare_rack_count_pairs);
  qsort(new_pairs, n_new, sizeof(RackCountPair), compare_rack_count_pairs);

  bool match = true;

  if (n_legacy != n_new) {
    if (verbose) {
      printf("  MISMATCH: legacy has %u items, new has %u items\n", n_legacy, n_new);
    }
    match = false;
  }

  if (am_legacy->total_item_count != am_new->total_item_count) {
    if (verbose) {
      printf("  MISMATCH: legacy total_count=%llu, new total_count=%llu\n",
             (unsigned long long)am_legacy->total_item_count,
             (unsigned long long)am_new->total_item_count);
    }
    match = false;
  }

  uint32_t i_legacy = 0, i_new = 0;
  int ld_size = ld_get_size(ld);
  Rack rack;
  rack_set_dist_size(&rack, ld_size);

  while (i_legacy < n_legacy && i_new < n_new) {
    int cmp = compare_rack_count_pairs(&legacy_pairs[i_legacy], &new_pairs[i_new]);
    if (cmp == 0) {
      if (legacy_pairs[i_legacy].count != new_pairs[i_new].count) {
        if (verbose) {
          rack_decode(&legacy_pairs[i_legacy].rack, &rack);
          StringBuilder *sb = string_builder_create();
          string_builder_add_rack(sb, &rack, ld, false);
          printf("  MISMATCH: rack %s has legacy_count=%u, new_count=%u\n",
                 string_builder_peek(sb), legacy_pairs[i_legacy].count,
                 new_pairs[i_new].count);
          string_builder_destroy(sb);
        }
        match = false;
      }
      i_legacy++;
      i_new++;
    } else if (cmp < 0) {
      if (verbose) {
        rack_decode(&legacy_pairs[i_legacy].rack, &rack);
        StringBuilder *sb = string_builder_create();
        string_builder_add_rack(sb, &rack, ld, false);
        printf("  MISMATCH: rack %s in LEGACY only (count=%u)\n",
               string_builder_peek(sb), legacy_pairs[i_legacy].count);
        string_builder_destroy(sb);
      }
      match = false;
      i_legacy++;
    } else {
      if (verbose) {
        rack_decode(&new_pairs[i_new].rack, &rack);
        StringBuilder *sb = string_builder_create();
        string_builder_add_rack(sb, &rack, ld, false);
        printf("  MISMATCH: rack %s in NEW only (count=%u)\n",
               string_builder_peek(sb), new_pairs[i_new].count);
        string_builder_destroy(sb);
      }
      match = false;
      i_new++;
    }
  }

  while (i_legacy < n_legacy) {
    if (verbose) {
      rack_decode(&legacy_pairs[i_legacy].rack, &rack);
      StringBuilder *sb = string_builder_create();
      string_builder_add_rack(sb, &rack, ld, false);
      printf("  MISMATCH: rack %s in LEGACY only (count=%u)\n",
             string_builder_peek(sb), legacy_pairs[i_legacy].count);
      string_builder_destroy(sb);
    }
    match = false;
    i_legacy++;
  }
  while (i_new < n_new) {
    if (verbose) {
      rack_decode(&new_pairs[i_new].rack, &rack);
      StringBuilder *sb = string_builder_create();
      string_builder_add_rack(sb, &rack, ld, false);
      printf("  MISMATCH: rack %s in NEW only (count=%u)\n",
             string_builder_peek(sb), new_pairs[i_new].count);
      string_builder_destroy(sb);
    }
    match = false;
    i_new++;
  }

  free(legacy_pairs);
  free(new_pairs);

  return match;
}

// Run inference with given parameters
// Returns AliasMethod (caller must destroy)
// Returns NULL if inference fails (e.g., played tiles not in bag)
static AliasMethod *run_inference_for_move(Config *config, const Game *game,
                                           int target_index,
                                           int target_score,
                                           int target_num_exch,
                                           Rack *target_played_tiles,
                                           int equity_margin,
                                           bool use_new_algo) {
  const LetterDistribution *ld = game_get_ld(game);
  int ld_size = ld_get_size(ld);

  AliasMethod *am = alias_method_create();
  InferenceResults *results = inference_results_create(am);
  ErrorStack *error_stack = error_stack_create();

  InferenceArgs args;
  memset(&args, 0, sizeof(InferenceArgs));
  args.game = game;
  args.target_index = target_index;
  args.target_score = int_to_equity(target_score);
  args.target_num_exch = target_num_exch;
  args.equity_margin = int_to_equity(equity_margin);
  args.move_capacity = 10;
  args.all_unseen_inference_movegen = use_new_algo;
  args.num_threads = 1;
  args.thread_control = config_get_thread_control(config);
  args.target_played_tiles = target_played_tiles;

  Rack empty_rack;
  rack_set_dist_size_and_reset(&empty_rack, ld_size);
  args.target_known_rack = &empty_rack;
  args.nontarget_known_rack = &empty_rack;

  infer(&args, results, error_stack);

  if (!error_stack_is_empty(error_stack)) {
    // Inference failed - clean up and return NULL
    error_stack_destroy(error_stack);
    inference_results_destroy(results);
    return NULL;
  }

  error_stack_destroy(error_stack);
  inference_results_destroy(results);

  return am;
}

// Extract played tiles from a move into a Rack
static void get_played_tiles_from_move(const Move *move, const Game *game,
                                       Rack *played_tiles) {
  const LetterDistribution *ld = game_get_ld(game);
  int ld_size = ld_get_size(ld);
  rack_set_dist_size_and_reset(played_tiles, ld_size);

  game_event_t move_type = move_get_type(move);
  if (move_type == GAME_EVENT_TILE_PLACEMENT_MOVE) {
    int tiles_length = move_get_tiles_length(move);
    for (int i = 0; i < tiles_length; i++) {
      MachineLetter ml = move_get_tile(move, i);
      if (ml == PLAYED_THROUGH_MARKER) {
        continue;
      }
      if (get_is_blanked(ml)) {
        rack_add_letter(played_tiles, BLANK_MACHINE_LETTER);
      } else {
        rack_add_letter(played_tiles, ml);
      }
    }
  } else if (move_type == GAME_EVENT_EXCHANGE) {
    int tiles_played = move_get_tiles_played(move);
    for (int i = 0; i < tiles_played; i++) {
      MachineLetter ml = move_get_tile(move, i);
      rack_add_letter(played_tiles, ml);
    }
  }
}

// Play through one complete game comparing inference at each turn
// Returns number of mismatches found
// Accumulates timing in total_legacy_time_ms and total_new_time_ms
static int play_game_with_inference_comparison(Config *config,
                                               int game_number,
                                               uint64_t seed,
                                               int *total_comparisons,
                                               double *total_legacy_time_ms,
                                               double *total_new_time_ms) {
  Game *game = config_get_game(config);
  const LetterDistribution *ld = game_get_ld(game);
  int ld_size = ld_get_size(ld);
  Bag *bag = game_get_bag(game);

  // Reset game with specific seed
  game_reset(game);
  bag_seed(bag, seed);
  draw_starting_racks(game);

  MoveList *move_list = move_list_create(1);
  Rack played_tiles;
  rack_set_dist_size(&played_tiles, ld_size);

  int turn = 0;
  int mismatches = 0;

  while (!game_over(game)) {
    int player_on_turn = game_get_player_on_turn_index(game);

    // Generate best move
    Move *move = get_top_equity_move(game, 0, move_list);
    game_event_t move_type = move_get_type(move);

    // Only compare inference for scoring plays (not exchanges or passes)
    // Exchanges require different handling and passes have no played tiles
    if (move_type == GAME_EVENT_TILE_PLACEMENT_MOVE) {
      int score = equity_to_int(move_get_score(move));
      get_played_tiles_from_move(move, game, &played_tiles);

      // Run legacy inference with timing
      clock_t legacy_start = clock();
      AliasMethod *am_legacy = run_inference_for_move(
          config, game, player_on_turn, score, 0, &played_tiles, 50, false);
      clock_t legacy_end = clock();
      *total_legacy_time_ms += (double)(legacy_end - legacy_start) * 1000.0 / CLOCKS_PER_SEC;

      // Run new inference with timing
      clock_t new_start = clock();
      AliasMethod *am_new = run_inference_for_move(
          config, game, player_on_turn, score, 0, &played_tiles, 50, true);
      clock_t new_end = clock();
      *total_new_time_ms += (double)(new_end - new_start) * 1000.0 / CLOCKS_PER_SEC;

      if (am_legacy && am_new) {
        (*total_comparisons)++;
        bool match = compare_alias_methods(am_legacy, am_new, ld, false);
        if (!match) {
          mismatches++;
          printf("MISMATCH in game %d, turn %d:\n", game_number + 1, turn + 1);

          StringBuilder *sb = string_builder_create();
          string_builder_add_move(sb, game_get_board(game), move, ld);
          printf("  Move: %s (score %d)\n", string_builder_peek(sb), score);
          string_builder_destroy(sb);

          sb = string_builder_create();
          string_builder_add_rack(sb, &played_tiles, ld, false);
          printf("  Played tiles: %s\n", string_builder_peek(sb));
          string_builder_destroy(sb);

          printf("  Legacy: %u items, %llu total\n",
                 am_legacy->num_items,
                 (unsigned long long)am_legacy->total_item_count);
          printf("  New: %u items, %llu total\n",
                 am_new->num_items,
                 (unsigned long long)am_new->total_item_count);

          // Print detailed differences
          compare_alias_methods(am_legacy, am_new, ld, true);
        }
      }

      if (am_legacy) alias_method_destroy(am_legacy);
      if (am_new) alias_method_destroy(am_new);
    }

    // Play the move
    play_move(move, game, NULL);
    turn++;
  }

  move_list_destroy(move_list);
  return mismatches;
}

// Main test: play games and compare inference at every turn
static void test_compare_full_games(int num_games) {
  printf("=== Comparing inference over %d full games ===\n", num_games);

  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);

  int total_mismatches = 0;
  int total_comparisons = 0;
  double total_legacy_time_ms = 0.0;
  double total_new_time_ms = 0.0;

  for (int i = 0; i < num_games; i++) {
    if ((i + 1) % 10 == 0 || i == 0) {
      printf("  Playing game %d/%d...\n", i + 1, num_games);
    }

    // Use different seed for each game
    uint64_t seed = 12345 + (uint64_t)i * 1000;

    int mismatches = play_game_with_inference_comparison(
        config, i, seed, &total_comparisons,
        &total_legacy_time_ms, &total_new_time_ms);
    total_mismatches += mismatches;
  }

  printf("\n=== RESULTS ===\n");
  printf("Games played: %d\n", num_games);
  printf("Inference comparisons: %d\n", total_comparisons);
  printf("Mismatches found: %d\n", total_mismatches);
  printf("\n=== TIMING ===\n");
  printf("Legacy algorithm total: %.2f ms (%.2f ms avg)\n",
         total_legacy_time_ms, total_legacy_time_ms / total_comparisons);
  printf("New algorithm total:    %.2f ms (%.2f ms avg)\n",
         total_new_time_ms, total_new_time_ms / total_comparisons);
  printf("Speedup: %.2fx\n", total_legacy_time_ms / total_new_time_ms);

  if (total_mismatches == 0) {
    printf("PASSED: All inference results match!\n\n");
  } else {
    printf("FAILED: Found %d mismatches!\n\n", total_mismatches);
  }

  assert(total_mismatches == 0);

  config_destroy(config);
}

// =============================================================================
// Main test function
// =============================================================================
void inference_comparison_test(void) {
  printf("\n========================================\n");
  printf("Running inference_comparison_test\n");
  printf("========================================\n\n");

  test_compare_full_games(100);

  printf("========================================\n");
  printf("All inference_comparison tests passed!\n");
  printf("========================================\n\n");
}
