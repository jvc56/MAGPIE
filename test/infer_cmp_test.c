#include "../src/def/board_defs.h"
#include "../src/def/game_defs.h"
#include "../src/def/inference_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/inference_results.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/rack.h"
#include "../src/ent/stats.h"
#include "../src/ent/thread_control.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/util/io_util.h"
#include "test_constants.h"
#include "test_util.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// Helper function to compare inference results between optimized and
// non-optimized runs
static void
assert_inference_results_match(InferenceResults *results_with_cutoff,
                               InferenceResults *results_without_cutoff,
                               int ld_size) {
  // Compare equity values stats for LEAVE type
  const Stat *eq_with = inference_results_get_equity_values(
      results_with_cutoff, INFERENCE_TYPE_LEAVE);
  const Stat *eq_without = inference_results_get_equity_values(
      results_without_cutoff, INFERENCE_TYPE_LEAVE);

  assert(stat_get_num_samples(eq_with) == stat_get_num_samples(eq_without));
  assert(stat_get_num_unique_samples(eq_with) ==
         stat_get_num_unique_samples(eq_without));
  if (stat_get_num_samples(eq_with) > 0) {
    assert(within_epsilon(stat_get_mean(eq_with), stat_get_mean(eq_without)));
  }

  // Compare subtotals for each letter
  for (int ml = 0; ml < ld_size; ml++) {
    for (int num_letters = 1; num_letters <= RACK_SIZE; num_letters++) {
      uint64_t draw_with = inference_results_get_subtotal(
          results_with_cutoff, INFERENCE_TYPE_LEAVE, ml, num_letters,
          INFERENCE_SUBTOTAL_DRAW);
      uint64_t draw_without = inference_results_get_subtotal(
          results_without_cutoff, INFERENCE_TYPE_LEAVE, ml, num_letters,
          INFERENCE_SUBTOTAL_DRAW);
      assert(draw_with == draw_without);

      uint64_t leave_with = inference_results_get_subtotal(
          results_with_cutoff, INFERENCE_TYPE_LEAVE, ml, num_letters,
          INFERENCE_SUBTOTAL_LEAVE);
      uint64_t leave_without = inference_results_get_subtotal(
          results_without_cutoff, INFERENCE_TYPE_LEAVE, ml, num_letters,
          INFERENCE_SUBTOTAL_LEAVE);
      assert(leave_with == leave_without);
    }
  }
}

// Run inference with and without cutoff optimization and verify results match
static void run_inference_comparison(const Config *config, int target_index,
                                     Equity target_score, int target_num_exch,
                                     Rack *target_played_tiles,
                                     InferenceResults *results_with_cutoff,
                                     InferenceResults *results_without_cutoff,
                                     int ld_size, ErrorStack *error_stack) {
  Rack target_known_rack;
  rack_set_dist_size_and_reset(&target_known_rack, ld_size);
  Rack nontarget_known_rack;
  rack_set_dist_size_and_reset(&nontarget_known_rack, ld_size);

  // Run inference WITH cutoff optimization
  thread_control_set_status(config_get_thread_control(config),
                            THREAD_CONTROL_STATUS_STARTED);
  config_infer(config, false, target_index, target_score, target_num_exch,
               target_played_tiles, &target_known_rack, &nontarget_known_rack,
               true, results_with_cutoff, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  // Run inference WITHOUT cutoff optimization
  thread_control_set_status(config_get_thread_control(config),
                            THREAD_CONTROL_STATUS_STARTED);
  config_infer(config, false, target_index, target_score, target_num_exch,
               target_played_tiles, &target_known_rack, &nontarget_known_rack,
               false, results_without_cutoff, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  // Verify results match
  assert_inference_results_match(results_with_cutoff, results_without_cutoff,
                                 ld_size);
}

// Test that cutoff optimization produces identical results to non-optimized
// inference. Runs games until we have at least MIN_FULL_GAMES complete and
// have tested at least MIN_EXCHANGES_ABOVE_THRESHOLD exchanges where the
// optimization is enabled (5+ tiles) and MIN_EXCHANGES_BELOW_THRESHOLD where
// it's disabled (1-4 tiles). Has a hard cap of MAX_GAMES to prevent CI
// timeouts.
static void test_infer_cutoff_optimization(bool use_wmp) {
  const int NUM_THREADS = 10;
  const int MIN_FULL_GAMES = 5;
  const int MIN_EXCHANGES_ABOVE_THRESHOLD = 2;
  const int MIN_EXCHANGES_BELOW_THRESHOLD = 2;
  const int MAX_GAMES = 100;

  char *config_str = get_formatted_string(
      "set -lex CSW21 -wmp %s -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1 -threads %d",
      use_wmp ? "true" : "false", NUM_THREADS);
  Config *config = config_create_or_die(config_str);
  free(config_str);

  const LetterDistribution *ld = config_get_ld(config);
  const int ld_size = ld_get_size(ld);

  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  Game *game = config_get_game(config);

  InferenceResults *results_with_cutoff = inference_results_create(NULL);
  InferenceResults *results_without_cutoff = inference_results_create(NULL);
  ErrorStack *error_stack = error_stack_create();

  MoveList *move_list = move_list_create(1);

  int games_completed = 0;
  int exchanges_above_threshold = 0; // optimization enabled (5+ tiles)
  int exchanges_below_threshold = 0; // optimization disabled (1-4 tiles)
  int scoring_plays_tested = 0;
  const uint64_t base_seed = 42;

  while ((games_completed < MIN_FULL_GAMES ||
          exchanges_above_threshold < MIN_EXCHANGES_ABOVE_THRESHOLD ||
          exchanges_below_threshold < MIN_EXCHANGES_BELOW_THRESHOLD) &&
         games_completed < MAX_GAMES) {
    // Reset game and seed for deterministic behavior
    game_reset(game);
    game_seed(game, base_seed + games_completed);
    draw_starting_racks(game);

    // Play the game move by move
    while (!game_over(game)) {
      const int player_on_turn = game_get_player_on_turn_index(game);
      const Bag *bag = game_get_bag(game);

      // Generate the top equity move
      const Move *top_move = get_top_equity_move(game, 0, move_list);
      const game_event_t move_type = move_get_type(top_move);

      // Only test inference for tile placement moves and exchanges
      // when there are enough tiles in the bag for inference to be valid
      if ((move_type == GAME_EVENT_TILE_PLACEMENT_MOVE ||
           move_type == GAME_EVENT_EXCHANGE) &&
          bag_get_letters(bag) >= RACK_SIZE) {

        const Equity score = move_get_score(top_move);
        const int tiles_played = move_get_tiles_played(top_move);

        int num_exch = 0;
        Rack target_played_tiles;
        rack_set_dist_size_and_reset(&target_played_tiles, ld_size);

        if (move_type == GAME_EVENT_EXCHANGE) {
          num_exch = tiles_played;
        } else {
          // For scoring plays, extract the tiles played
          for (int i = 0; i < tiles_played; i++) {
            const MachineLetter ml = move_get_tile(top_move, i);
            if (ml != PLAYED_THROUGH_MARKER) {
              rack_add_letter(&target_played_tiles,
                              get_unblanked_machine_letter(ml));
            }
          }
        }

        // Run inference comparison
        run_inference_comparison(config, player_on_turn, score, num_exch,
                                 &target_played_tiles, results_with_cutoff,
                                 results_without_cutoff, ld_size, error_stack);

        if (!error_stack_is_empty(error_stack)) {
          // Some inferences may fail, skip them
          error_stack_print_and_reset(error_stack);
        } else {
          // Track exchange categories
          if (move_type == GAME_EVENT_EXCHANGE) {
            const int leave_size = RACK_SIZE - num_exch;
            if (leave_size >= INFERENCE_CUTOFF_MIN_EXCHANGE_LEAVE_SIZE) {
              // Optimization disabled for small exchanges (leave size >= 3)
              exchanges_below_threshold++;
            } else {
              // Optimization enabled for large exchanges (leave size < 3)
              exchanges_above_threshold++;
            }
          } else {
            scoring_plays_tested++;
          }
        }
      }

      // Play the move
      play_move(top_move, game, NULL);
    }

    games_completed++;
  }

  printf("Cutoff optimization test (%s): %d games, %d scoring plays, "
         "%d exchanges above threshold, %d exchanges below threshold\n",
         use_wmp ? "WMP" : "KWG", games_completed, scoring_plays_tested,
         exchanges_above_threshold, exchanges_below_threshold);

  move_list_destroy(move_list);
  error_stack_destroy(error_stack);
  inference_results_destroy(results_with_cutoff);
  inference_results_destroy(results_without_cutoff);
  config_destroy(config);
}

void test_infer_cmp(void) {
  // Test cutoff optimization for both WMP and KWG modes
  test_infer_cutoff_optimization(true);  // WMP
  test_infer_cutoff_optimization(false); // KWG
}