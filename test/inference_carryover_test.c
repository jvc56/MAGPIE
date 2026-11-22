#include "inference_carryover_test.h"

#include "../src/ent/alias_method.h"
#include "../src/ent/encoded_rack.h"

#include "../src/def/game_history_defs.h"
#include "../src/def/inference_defs.h"
#include "../src/def/klv_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/game_history.h"
#include "../src/ent/inference_args.h"
#include "../src/ent/inference_carryover.h"
#include "../src/ent/inference_results.h"
#include "../src/ent/klv.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/stats.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/validated_move.h"
#include "../src/impl/config.h"
#include "../src/impl/inference.h"
#include "../src/str/rack_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_constants.h"
#include "test_util.h"
#include <assert.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  CARRY_TEST_LEAVE_STRING_LENGTH = 32,
};

static void carry_test_set_slot(InferenceCarryoverSlot *slots, const KLV *klv,
                                const LetterDistribution *ld,
                                const char *leave_string, float weight) {
  Rack leave;
  const int num_tiles = rack_set_to_string(ld, &leave, leave_string);
  assert(num_tiles > 0);
  const uint32_t leave_index = klv_get_word_index(klv, &leave);
  assert(leave_index != KLV_UNFOUND_INDEX);
  uint32_t bits;
  memcpy(&bits, &weight, sizeof(bits));
  atomic_store(&slots[leave_index], bits);
}

static float carry_test_get_weight(const InferenceCarryoverTable *table,
                                   const KLV *klv, const LetterDistribution *ld,
                                   const char *leave_string) {
  Rack leave;
  const int num_tiles = rack_set_to_string(ld, &leave, leave_string);
  assert(num_tiles > 0);
  return inference_carryover_table_get_weight(table,
                                              klv_get_word_index(klv, &leave));
}

static void carry_test_assert_tables_equal(const InferenceCarryoverTable *left,
                                           const InferenceCarryoverTable *right,
                                           int dist_size) {
  assert(inference_carryover_table_get_kept(left) ==
         inference_carryover_table_get_kept(right));
  assert(inference_carryover_table_get_count(left) ==
         inference_carryover_table_get_count(right));
  assert(inference_carryover_table_get_max(left) ==
         inference_carryover_table_get_max(right));
  Rack left_leave;
  Rack right_leave;
  for (uint32_t entry_idx = 0;
       entry_idx < inference_carryover_table_get_count(left); entry_idx++) {
    assert(inference_carryover_table_get_entry_index(left, entry_idx) ==
           inference_carryover_table_get_entry_index(right, entry_idx));
    assert(inference_carryover_table_get_entry_weight(left, entry_idx) ==
           inference_carryover_table_get_entry_weight(right, entry_idx));
    inference_carryover_table_get_entry_leave(left, entry_idx, dist_size,
                                              &left_leave);
    inference_carryover_table_get_entry_leave(right, entry_idx, dist_size,
                                              &right_leave);
    assert(racks_are_equal(&left_leave, &right_leave));
  }
}

// A table is the positive slots, with each leave's tiles recovered from its
// index; it survives a round trip through its text file.
static void test_carry_table(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  const Game *game = config_get_game(config);
  const LetterDistribution *ld = game_get_ld(game);
  const int dist_size = ld_get_size(ld);
  const KLV *klv = player_get_klv(game_get_player(game, 0));
  const uint32_t num_leaves = klv_get_number_of_leaves(klv);
  InferenceCarryoverSlot *slots = calloc_or_die(num_leaves, sizeof(*slots));

  assert(inference_carryover_table_create_from_slots(slots, klv, 3) == NULL);

  const char *leaves[] = {"AEN", "?AE", "QUZ", "Z??", "EEE", "RST"};
  const float weights[] = {0.5F, 0.25F, 2.0F, 0.125F, 1.5F, 0.75F};
  const int num_test_leaves = (int)(sizeof(leaves) / sizeof(leaves[0]));
  for (int leave_idx = 0; leave_idx < num_test_leaves; leave_idx++) {
    carry_test_set_slot(slots, klv, ld, leaves[leave_idx], weights[leave_idx]);
  }
  assert(inference_carryover_table_create_from_slots(slots, klv, 0) == NULL);
  assert(inference_carryover_table_create_from_slots(slots, klv, RACK_SIZE) ==
         NULL);
  // The slots hold leaves of three tiles, so they are not a table of four.
  assert(inference_carryover_table_create_from_slots(slots, klv, 4) == NULL);

  InferenceCarryoverTable *table =
      inference_carryover_table_create_from_slots(slots, klv, 3);
  assert(table != NULL);
  assert(inference_carryover_table_get_kept(table) == 3);
  assert(inference_carryover_table_get_count(table) ==
         (uint32_t)num_test_leaves);
  assert(inference_carryover_table_get_num_leaves(table) == num_leaves);
  assert(inference_carryover_table_get_max(table) == 2.0F);
  for (int leave_idx = 0; leave_idx < num_test_leaves; leave_idx++) {
    assert(carry_test_get_weight(table, klv, ld, leaves[leave_idx]) ==
           weights[leave_idx]);
  }
  assert(carry_test_get_weight(table, klv, ld, "AEI") == 0);
  assert(carry_test_get_weight(table, klv, ld, "AE") == 0);

  // Each entry's tiles are the leave its index names.
  Rack entry_leave;
  for (uint32_t entry_idx = 0;
       entry_idx < inference_carryover_table_get_count(table); entry_idx++) {
    inference_carryover_table_get_entry_leave(table, entry_idx, dist_size,
                                              &entry_leave);
    assert(rack_get_total_letters(&entry_leave) == 3);
    assert(klv_get_word_index(klv, &entry_leave) ==
           inference_carryover_table_get_entry_index(table, entry_idx));
    if (entry_idx > 0) {
      assert(inference_carryover_table_get_entry_index(table, entry_idx - 1) <
             inference_carryover_table_get_entry_index(table, entry_idx));
    }
  }

  // The floor only counts leaves the target can still hold: with no Q and one
  // E unseen, QUZ and EEE are out.
  Rack pool;
  rack_set_to_string(ld, &pool, "AENRSTZ");
  assert(inference_carryover_table_get_max_within(table, &pool) == 0.75F);
  rack_set_to_string(ld, &pool, "??ZAEEE");
  assert(inference_carryover_table_get_max_within(table, &pool) == 1.5F);
  rack_set_to_string(ld, &pool, "BCD");
  assert(inference_carryover_table_get_max_within(table, &pool) == 0);

  // Filling slots clears what was there.
  InferenceCarryoverSlot *filled = calloc_or_die(num_leaves, sizeof(*filled));
  carry_test_set_slot(filled, klv, ld, "AEI", 9.0F);
  inference_carryover_table_fill_slots(table, filled);
  InferenceCarryoverTable *refilled =
      inference_carryover_table_create_from_slots(filled, klv, 3);
  carry_test_assert_tables_equal(table, refilled, dist_size);
  inference_carryover_table_destroy(refilled);
  free(filled);

  InferenceCarryoverTable *duplicate =
      inference_carryover_table_duplicate(table);
  carry_test_assert_tables_equal(table, duplicate, dist_size);
  inference_carryover_table_destroy(duplicate);
  assert(inference_carryover_table_duplicate(NULL) == NULL);

  const char *filename = "inference_carryover_test.tsv";
  assert(inference_carryover_table_write(table, ld, "a label", filename));
  InferenceCarryoverTable *read_back =
      inference_carryover_table_read(klv, ld, filename);
  assert(read_back != NULL);
  carry_test_assert_tables_equal(table, read_back, dist_size);
  inference_carryover_table_destroy(read_back);
  (void)remove(filename);
  assert(inference_carryover_table_read(klv, ld, filename) == NULL);

  inference_carryover_table_destroy(table);
  free(slots);
  config_destroy(config);
}

static void test_carry_chain(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  const Game *game = config_get_game(config);
  const LetterDistribution *ld = game_get_ld(game);
  const KLV *klv = player_get_klv(game_get_player(game, 0));
  InferenceCarryoverSlot *slots =
      calloc_or_die(klv_get_number_of_leaves(klv), sizeof(*slots));
  carry_test_set_slot(slots, klv, ld, "AB", 1.0F);

  InferenceCarryoverChain *chain = inference_carryover_chain_create();
  assert(inference_carryover_chain_get_table(chain) == NULL);
  assert(inference_carryover_chain_get_length(chain) == 0);
  InferenceCarryoverTable *first =
      inference_carryover_table_create_from_slots(slots, klv, 2);
  inference_carryover_chain_advance(chain, first);
  assert(inference_carryover_chain_get_table(chain) == first);
  assert(inference_carryover_chain_get_length(chain) == 1);
  InferenceCarryoverTable *second =
      inference_carryover_table_create_from_slots(slots, klv, 2);
  inference_carryover_chain_advance(chain, second);
  assert(inference_carryover_chain_get_table(chain) == second);
  assert(inference_carryover_chain_get_length(chain) == 2);
  // An inference that recorded nothing (a bingo) ends the chain.
  inference_carryover_chain_advance(chain, NULL);
  assert(inference_carryover_chain_get_table(chain) == NULL);
  assert(inference_carryover_chain_get_length(chain) == 0);
  inference_carryover_chain_advance(
      chain, inference_carryover_table_create_from_slots(slots, klv, 2));
  inference_carryover_chain_reset(chain);
  assert(inference_carryover_chain_get_table(chain) == NULL);
  assert(inference_carryover_chain_get_length(chain) == 0);
  inference_carryover_chain_destroy(chain);
  inference_carryover_chain_destroy(NULL);
  free(slots);
  config_destroy(config);
}

static const InferenceCarryoverTable *
carry_test_event_table(const Config *config, int event_index) {
  return game_event_get_inference_carryover(
      game_history_get_event(config_get_game_history(config), event_index));
}

// The table the last inference kept is what an inference of the same move
// records when the table before it is handed over through the harness's
// carry_in and carry_out, with the floor taken from the leaves the target can
// still hold.
static void carry_test_assert_matches_by_hand(Config *config) {
  GameHistory *history = config_get_game_history(config);
  const int event_index =
      game_history_get_most_recent_move_event_index(history);
  const GameEvent *event = game_history_get_event(history, event_index);
  const InferenceCarryoverTable *table =
      game_event_get_inference_carryover(event);
  const InferenceCarryoverTable *table_in =
      game_history_get_inference_carryover_in(history, event_index);
  assert(table != NULL);
  assert(table_in != NULL);
  const Game *game = config_get_game(config);
  const LetterDistribution *ld = game_get_ld(game);
  const int dist_size = ld_get_size(ld);
  const int target_index = game_event_get_player_index(event);
  const KLV *klv = player_get_klv(game_get_player(game, target_index));
  const uint32_t num_leaves = klv_get_number_of_leaves(klv);

  // What the observer had not seen before the move: the bag and the target's
  // rack now, and the tiles the move put on the board.
  Rack pool;
  rack_set_dist_size_and_reset(&pool, dist_size);
  const Rack *target_rack =
      player_get_rack(game_get_player(game, target_index));
  for (int ml = 0; ml < dist_size; ml++) {
    rack_add_letters(&pool, ml,
                     bag_get_letter(game_get_bag(game), ml) +
                         rack_get_letter(target_rack, ml));
  }
  const Move *move = validated_moves_get_move(game_event_get_vms(event), 0);
  for (int tile_idx = 0; tile_idx < move_get_tiles_length(move); tile_idx++) {
    const MachineLetter ml = move_get_tile(move, tile_idx);
    if (move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE &&
        ml != PLAYED_THROUGH_MARKER) {
      rack_add_letter(&pool, get_is_blanked(ml) ? BLANK_MACHINE_LETTER : ml);
    }
  }

  InferenceCarryoverSlot *slots_in =
      calloc_or_die(num_leaves, sizeof(*slots_in));
  InferenceCarryoverSlot *slots_out =
      calloc_or_die(num_leaves, sizeof(*slots_out));
  inference_carryover_table_fill_slots(table_in, slots_in);
  Rack target_played_tiles;
  rack_set_dist_size_and_reset(&target_played_tiles, dist_size);
  Rack target_known_rack;
  rack_set_dist_size_and_reset(&target_known_rack, dist_size);
  Rack nontarget_known_rack;
  rack_set_dist_size_and_reset(&nontarget_known_rack, dist_size);
  InferenceArgs args;
  // The settings of the game's config: temperature 8 with no margin.
  infer_args_fill(&args, 1, int_to_equity(10000), history, game, 1, 0, 0,
                  config_get_thread_control(config), true, true, 0, 0, 0,
                  &target_played_tiles, &target_known_rack,
                  &nontarget_known_rack);
  args.temperature = int_to_equity(8);
  args.carry_in = slots_in;
  args.carry_in_kept = inference_carryover_table_get_kept(table_in);
  args.carry_in_floor =
      INFERENCE_CARRYOVER_FLOOR_FRACTION *
      (double)inference_carryover_table_get_max_within(table_in, &pool);
  args.carry_out = slots_out;
  InferenceResults *results = inference_results_create(NULL);
  ErrorStack *error_stack = error_stack_create();
  thread_control_set_status(config_get_thread_control(config),
                            THREAD_CONTROL_STATUS_STARTED);
  infer_without_ctx(&args, results, error_stack);
  assert(error_stack_is_empty(error_stack));
  InferenceCarryoverTable *by_hand =
      inference_carryover_table_create_from_slots(
          slots_out, klv, inference_carryover_table_get_kept(table));
  assert(by_hand != NULL);
  carry_test_assert_tables_equal(table, by_hand, dist_size);
  // The table that came in changed what was recorded.
  memset(slots_out, 0, (size_t)num_leaves * sizeof(*slots_out));
  args.carry_in = NULL;
  args.carry_in_kept = 0;
  args.carry_in_floor = 0;
  thread_control_set_status(config_get_thread_control(config),
                            THREAD_CONTROL_STATUS_STARTED);
  infer_without_ctx(&args, results, error_stack);
  assert(error_stack_is_empty(error_stack));
  InferenceCarryoverTable *without =
      inference_carryover_table_create_from_slots(
          slots_out, klv, inference_carryover_table_get_kept(table));
  assert(without != NULL);
  assert(inference_carryover_table_get_count(without) ==
         inference_carryover_table_get_count(table));
  assert(inference_carryover_table_get_max(without) !=
         inference_carryover_table_get_max(table));
  inference_carryover_table_destroy(without);
  inference_carryover_table_destroy(by_hand);
  error_stack_destroy(error_stack);
  inference_results_destroy(results);
  free(slots_out);
  free(slots_in);
}

// A game played through the commands, each move inferred from the game
// history before the reply: the tables land on the events of the moves they
// describe, and the look-back follows the rules for passes and bingos.
static void test_carry_game_history(bool carry_over) {
  // The tests' default inference margin is 0; softmax inference in play has
  // no margin to speak of (the config's default is 10000).
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1 -threads 1 -inftemp 8 -ima 10000 -fgrequired false");
  load_and_exec_config_or_die(config, carry_over ? "set -infcarryover true"
                                                 : "set -infcarryover false");
  load_and_exec_config_or_die(config, "newgame");
  // Event 0, player 1: keeps AEN.
  load_and_exec_config_or_die(config, "rack QUIZAEN");
  load_and_exec_config_or_die(config, "com 8e QUIZ");
  load_and_exec_config_or_die(config, "rack MUDROTS");
  load_and_exec_config_or_die(config, "infer");
  // Event 1, player 2: keeps five tiles.
  load_and_exec_config_or_die(config, "com f7 M.D");
  load_and_exec_config_or_die(config, "rack AENLTRS");
  load_and_exec_config_or_die(config, "infer");
  // Event 2, player 1 passes: nothing to infer, nothing recorded.
  load_and_exec_config_or_die(config, "com pass");
  load_and_exec_config_or_die(config, "rack ORTSEAI");
  load_and_exec_config_or_die(config, "infer");
  // Event 3, player 2 exchanges two and keeps five.
  load_and_exec_config_or_die(config, "com ex OI");
  load_and_exec_config_or_die(config, "rack AENLTRS");
  load_and_exec_config_or_die(config, "infer");
  if (carry_over) {
    // The exchange, with the table of the play before it.
    carry_test_assert_matches_by_hand(config);
  }
  // Event 4, player 1 bingoes and keeps nothing.
  load_and_exec_config_or_die(config, "com 10f SALTERN");
  load_and_exec_config_or_die(config, "rack ERTSEAB");
  load_and_exec_config_or_die(config, "infer");
  // Event 5, player 2: keeps five.
  load_and_exec_config_or_die(config, "com j9 B.T");
  load_and_exec_config_or_die(config, "rack EIOUDGN");
  load_and_exec_config_or_die(config, "infer");
  if (carry_over) {
    // A play, with the table of the exchange before it.
    carry_test_assert_matches_by_hand(config);
  }
  // Event 6, player 1: keeps five.
  load_and_exec_config_or_die(config, "com l8 DO.");
  load_and_exec_config_or_die(config, "rack ERSECLP");
  load_and_exec_config_or_die(config, "infer");

  const GameHistory *history = config_get_game_history(config);
  assert(game_history_get_num_events(history) == 7);
  if (!carry_over) {
    for (int event_index = 0; event_index < 7; event_index++) {
      assert(carry_test_event_table(config, event_index) == NULL);
      assert(game_history_get_inference_carryover_in(history, event_index) ==
             NULL);
    }
    config_destroy(config);
    return;
  }
  const int expected_kept[] = {3, 5, 0, 5, 0, 5, 5};
  for (int event_index = 0; event_index < 7; event_index++) {
    const InferenceCarryoverTable *table =
        carry_test_event_table(config, event_index);
    if (expected_kept[event_index] == 0) {
      assert(table == NULL);
      continue;
    }
    assert(table != NULL);
    assert(inference_carryover_table_get_kept(table) ==
           expected_kept[event_index]);
    assert(inference_carryover_table_get_count(table) > 0);
    assert(inference_carryover_table_get_max(table) > 0);
  }
  // Each player's first move has nothing behind it.
  assert(game_history_get_inference_carryover_in(history, 0) == NULL);
  assert(game_history_get_inference_carryover_in(history, 1) == NULL);
  // Player 1's pass (event 2) and bingo (event 4) look back to QUIZ.
  assert(game_history_get_inference_carryover_in(history, 2) ==
         carry_test_event_table(config, 0));
  assert(game_history_get_inference_carryover_in(history, 4) ==
         carry_test_event_table(config, 0));
  // Player 2's exchange looks back to their play, and their next play to the
  // exchange.
  assert(game_history_get_inference_carryover_in(history, 3) ==
         carry_test_event_table(config, 1));
  assert(game_history_get_inference_carryover_in(history, 5) ==
         carry_test_event_table(config, 3));
  // The bingo kept nothing, so player 1's next move starts again.
  assert(game_history_get_inference_carryover_in(history, 6) == NULL);
  assert(game_history_get_inference_carryover_in(history, 7) == NULL);
  assert(game_history_get_inference_carryover_in(history, -1) == NULL);

  // QUIZ kept AEN: the true leave is in the table.
  const Game *game = config_get_game(config);
  const KLV *klv = player_get_klv(game_get_player(game, 0));
  assert(carry_test_get_weight(carry_test_event_table(config, 0), klv,
                               game_get_ld(game), "AEN") > 0);

  // A copy of the history has its own tables.
  GameHistory *history_copy = game_history_duplicate(history);
  const InferenceCarryoverTable *copied_table =
      game_event_get_inference_carryover(
          game_history_get_event(history_copy, 3));
  assert(copied_table != NULL);
  assert(copied_table != carry_test_event_table(config, 3));
  carry_test_assert_tables_equal(copied_table,
                                 carry_test_event_table(config, 3),
                                 ld_get_size(game_get_ld(game)));
  game_history_destroy(history_copy);

  // Inferring the same move again replaces its table with an equal one.
  InferenceCarryoverTable *before =
      inference_carryover_table_duplicate(carry_test_event_table(config, 6));
  load_and_exec_config_or_die(config, "infer");
  carry_test_assert_tables_equal(before, carry_test_event_table(config, 6),
                                 ld_get_size(game_get_ld(game)));
  // With several threads the workers add to a slot in any order, so the
  // weights agree to float rounding and not bit for bit.
  load_and_exec_config_or_die(config, "set -threads 4");
  load_and_exec_config_or_die(config, "infer");
  const InferenceCarryoverTable *threaded = carry_test_event_table(config, 6);
  assert(inference_carryover_table_get_count(threaded) ==
         inference_carryover_table_get_count(before));
  for (uint32_t entry_idx = 0;
       entry_idx < inference_carryover_table_get_count(before); entry_idx++) {
    assert(inference_carryover_table_get_entry_index(threaded, entry_idx) ==
           inference_carryover_table_get_entry_index(before, entry_idx));
    const double single =
        (double)inference_carryover_table_get_entry_weight(before, entry_idx);
    const double several =
        (double)inference_carryover_table_get_entry_weight(threaded, entry_idx);
    assert(fabs(several - single) <= 1e-4 * single);
  }
  inference_carryover_table_destroy(before);
  config_destroy(config);
}

// Plays the scripted game of test_carry_game_history, inferring each move as
// it is made or none of them.
static Config *carry_test_play_scripted_game(bool infer_each_move) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1 -threads 1 -inftemp 8 -ima 10000 -fgrequired false "
      "-infcarryover true");
  load_and_exec_config_or_die(config, "newgame");
  const char *racks[] = {"QUIZAEN", "MUDROTS", "AENLTRS", "ORTSEAI",
                         "AENLTRS", "ERTSEAB", "EIOUDGN", "ERSECLP"};
  const char *moves[] = {"com 8e QUIZ", "com f7 M.D",      "com pass",
                         "com ex OI",   "com 10f SALTERN", "com j9 B.T",
                         "com l8 DO."};
  for (int move_idx = 0; move_idx < 7; move_idx++) {
    char *rack_command = get_formatted_string("rack %s", racks[move_idx]);
    load_and_exec_config_or_die(config, rack_command);
    free(rack_command);
    load_and_exec_config_or_die(config, moves[move_idx]);
    if (infer_each_move) {
      rack_command = get_formatted_string("rack %s", racks[move_idx + 1]);
      load_and_exec_config_or_die(config, rack_command);
      free(rack_command);
      load_and_exec_config_or_die(config, "infer");
    }
  }
  if (!infer_each_move) {
    load_and_exec_config_or_die(config, "rack ERSECLP");
  }
  return config;
}

static bool carry_test_event_recorded(const Config *config, int event_index) {
  return game_event_get_inference_carryover_recorded(
      game_history_get_event(config_get_game_history(config), event_index));
}

// Jumping into a game that was never inferred fills in the moves behind the
// one inferred, back to the target's last full draw, and gives the tables
// that inferring every move in turn gives. Player 2's moves are events 1
// (M.D), 3 (the exchange) and 5 (B.T): a chain of three.
static void test_carry_backfill(void) {
  Config *played = carry_test_play_scripted_game(true);
  Config *jumped = carry_test_play_scripted_game(false);
  const int dist_size = ld_get_size(game_get_ld(config_get_game(played)));
  for (int event_index = 0; event_index < 7; event_index++) {
    assert(!carry_test_event_recorded(jumped, event_index));
    assert(carry_test_event_table(jumped, event_index) == NULL);
  }
  // Back to just after B.T, the third move of player 2's chain.
  load_and_exec_config_or_die(jumped, "previous");
  load_and_exec_config_or_die(jumped, "infer");
  const int chain_events[] = {1, 3, 5};
  for (int chain_idx = 0; chain_idx < 3; chain_idx++) {
    const int event_index = chain_events[chain_idx];
    assert(carry_test_event_recorded(jumped, event_index));
    assert(carry_test_event_table(jumped, event_index) != NULL);
    carry_test_assert_tables_equal(carry_test_event_table(played, event_index),
                                   carry_test_event_table(jumped, event_index),
                                   dist_size);
  }
  // Player 1's moves are not player 2's chain.
  assert(!carry_test_event_recorded(jumped, 0));
  assert(!carry_test_event_recorded(jumped, 2));
  assert(!carry_test_event_recorded(jumped, 4));

  // Inferring again reuses the tables behind: the same objects stay on the
  // events.
  const InferenceCarryoverTable *first_table =
      carry_test_event_table(jumped, 1);
  const InferenceCarryoverTable *second_table =
      carry_test_event_table(jumped, 3);
  load_and_exec_config_or_die(jumped, "infer");
  assert(carry_test_event_table(jumped, 1) == first_table);
  assert(carry_test_event_table(jumped, 3) == second_table);

  // Forward to player 1's DO.: the bingo before it was a full draw, so
  // nothing behind it is filled in. No later event holds player 2's rack, so
  // it is set again.
  load_and_exec_config_or_die(jumped, "next");
  load_and_exec_config_or_die(jumped, "rack ERSECLP");
  load_and_exec_config_or_die(jumped, "infer");
  assert(carry_test_event_recorded(jumped, 6));
  carry_test_assert_tables_equal(carry_test_event_table(played, 6),
                                 carry_test_event_table(jumped, 6), dist_size);
  assert(!carry_test_event_recorded(jumped, 0));
  assert(!carry_test_event_recorded(jumped, 4));

  // Back to just after the bingo (event 4): the pass is skipped and QUIZ,
  // the move before it, is filled in. The bingo itself keeps nothing.
  load_and_exec_config_or_die(jumped, "previous");
  load_and_exec_config_or_die(jumped, "previous");
  load_and_exec_config_or_die(jumped, "infer");
  assert(carry_test_event_recorded(jumped, 0));
  carry_test_assert_tables_equal(carry_test_event_table(played, 0),
                                 carry_test_event_table(jumped, 0), dist_size);
  assert(!carry_test_event_recorded(jumped, 2));
  assert(carry_test_event_recorded(jumped, 4));
  assert(carry_test_event_table(jumped, 4) == NULL);

  config_destroy(jumped);
  config_destroy(played);
}

// The probability the last inference gives to the target keeping exactly
// these tiles.
static double carry_test_leave_probability(Config *config,
                                           const char *leave_string) {
  const LetterDistribution *ld = game_get_ld(config_get_game(config));
  Rack leave;
  rack_set_dist_size_and_reset(&leave, ld_get_size(ld));
  const int num_tiles = rack_set_to_string(ld, &leave, leave_string);
  assert(num_tiles > 0);
  const AliasMethod *am = inference_results_get_alias_method(
      config_get_inference_results(config));
  Rack rack;
  rack_set_dist_size_and_reset(&rack, ld_get_size(ld));
  double total = 0;
  for (uint32_t idx = 0; idx < am->num_items; idx++) {
    const AliasMethodItem *item = &am->items[idx];
    rack_decode(&item->rack, &rack);
    if (racks_are_equal(&rack, &leave)) {
      total += item->probability / am->num_items;
    }
    rack_decode(&am->items[item->alias].rack, &rack);
    if (racks_are_equal(&rack, &leave)) {
      total += (1 - item->probability) / am->num_items;
    }
  }
  return total;
}

// A softmax inference of an exchange weighs every way of keeping tiles, not
// only each rack's best keep: a keep that is never the best one, such as QV
// from an exchange of five, still gets some probability, less than a good
// keep gets and less the lower the temperature.
static void test_soft_exchange_keep(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1 -threads 1 -inftemp 8 -ima 10000 -fgrequired false");
  load_and_exec_config_or_die(config, "newgame");
  load_and_exec_config_or_die(config, "rack QVUUWWI");
  load_and_exec_config_or_die(config, "com ex UUWWI");
  load_and_exec_config_or_die(config, "rack AENLTRD");
  load_and_exec_config_or_die(config, "infer");
  const double poor_keep = carry_test_leave_probability(config, "QV");
  const double good_keep = carry_test_leave_probability(config, "?S");
  assert(poor_keep > 0);
  assert(good_keep > poor_keep);
  // Each leave is handed to the sampler once, and they sum to one.
  const AliasMethod *am = inference_results_get_alias_method(
      config_get_inference_results(config));
  double total = 0;
  for (uint32_t idx = 0; idx < am->num_items; idx++) {
    total += am->items[idx].probability / am->num_items +
             (1 - am->items[idx].probability) / am->num_items;
  }
  assert(fabs(total - 1.0) < 1e-9);
  assert(am->num_items < 1000);

  // Several threads agree with one to float rounding.
  load_and_exec_config_or_die(config, "set -threads 4");
  load_and_exec_config_or_die(config, "infer");
  assert(fabs(carry_test_leave_probability(config, "QV") - poor_keep) <=
         1e-3 * poor_keep);

  // A sharper inference makes the poor keep less likely still, but possible.
  load_and_exec_config_or_die(config, "set -threads 1 -inftemp 2");
  load_and_exec_config_or_die(config, "infer");
  const double sharp_poor_keep = carry_test_leave_probability(config, "QV");
  assert(sharp_poor_keep > 0);
  assert(sharp_poor_keep < poor_keep);
  printf("soft exchange keep: QV %.3g at 8, %.3g at 2; ?S %.3g at 8\n",
         poor_keep, sharp_poor_keep, good_keep);
  config_destroy(config);
}

void test_inference_carryover(void) {
  test_soft_exchange_keep();
  test_carry_table();
  test_carry_chain();
  test_carry_game_history(false);
  test_carry_game_history(true);
  test_carry_backfill();
}

// Research probe (magpie_test infcarryoverprobe), not an assertion yet: an
// opponent burns a lone S for 1 point under a blank, then plays a Q for 62
// and an X for 50, one tile each, on a board set up for it. Prints the
// probability that their kept tiles include an S after each play, with the
// history off and on, at temperatures 8, 4, 2 and 1.6 (what the
// harness schedule gives with about 70 tiles in the bag, as here).
// MAGPIE_CARRY_FLOOR_PROBE overrides the floor fraction (see
// inference_carryover_begin): in CSW24 at temperature 1.6, with the default
// 1e-3 a belief over 99.99% falls to 36% after the Q; with 1e-6 it holds at
// 98.4%. No word on the board takes an S, so the 1-point hook is not passed
// over for a better one.
static double carry_test_probability_of_letter(Config *config,
                                               const char *letter) {
  InferenceResults *results = config_get_inference_results(config);
  const MachineLetter ml =
      ld_hl_to_ml(game_get_ld(config_get_game(config)), letter);
  const double total = stat_get_total_weight(
      inference_results_get_equity_values(results, INFERENCE_TYPE_LEAVE));
  const double with_letter =
      inference_results_get_weight_subtotal_sum_with_minimum(
          results, INFERENCE_TYPE_LEAVE, ml, 1);
  return with_letter / total;
}

void carry_probe(void) {
  const double temperatures[] = {8, 4, 2, 1.6};
  for (int temp_idx = 0; temp_idx < 4; temp_idx++) {
    for (int carry_over = 0; carry_over < 2; carry_over++) {
      char *settings = get_formatted_string(
          "set -lex CSW24 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
          "-numplays 1 -threads 4 -inftemp %g -ima 10000 -fgrequired false "
          "-infcarryover %s",
          temperatures[temp_idx], carry_over ? "true" : "false");
      Config *config = config_create_or_die(settings);
      free(settings);
      load_and_exec_config_or_die(
          config,
          "cgp 15/15/15/15/10M4/6I3U4/5INDRIs4/7O7/7Z7/7E7/7DaFT4/15/15/15/15 "
          "/ 0/0 0");
      const char *plays[] = {"com i11 .S", "com 6f Q.", "com 6j X."};
      const char *racks[] = {"EGILRSS", "EGILQRS", "EGILRSX"};
      for (int play_idx = 0; play_idx < 3; play_idx++) {
        char *rack_command = get_formatted_string("rack %s", racks[play_idx]);
        load_and_exec_config_or_die(config, rack_command);
        free(rack_command);
        load_and_exec_config_or_die(config, plays[play_idx]);
        load_and_exec_config_or_die(config, "rack AEGNOTW");
        load_and_exec_config_or_die(config, "infer");
        fprintf(stderr, "PROBE T=%g carry=%d after %s: score %d P(S)=%.4f\n",
                temperatures[temp_idx], carry_over, plays[play_idx],
                equity_to_int(game_event_get_move_score(game_history_get_event(
                    config_get_game_history(config), play_idx * 2))),
                carry_test_probability_of_letter(config, "S"));
        load_and_exec_config_or_die(config, "com pass");
      }
      config_destroy(config);
    }
  }
}
