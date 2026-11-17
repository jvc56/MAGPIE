#include "pool_shift_test.h"

#include "../src/compat/cfile.h"
#include "../src/def/board_defs.h"
#include "../src/def/equity_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/inference_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/best_move_carry.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/inference_args.h"
#include "../src/ent/inference_results.h"
#include "../src/ent/klv.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/players_data.h"
#include "../src/ent/pool_shift.h"
#include "../src/ent/rack.h"
#include "../src/ent/stats.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/validated_move.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/inference.h"
#include "../src/impl/move_gen.h"
#include "../src/str/move_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_constants.h"
#include "test_util.h"
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
  POOL_SHIFT_TEST_GAMES = 3,
  POOL_SHIFT_TEST_MOVE_LIST_CAPACITY = 1024,
  // The capacity of the top-n recording checked against all moves.
  POOL_SHIFT_TEST_TOP_N = 10,
  // The margin of the within-x-of-best recording checked, in points.
  POOL_SHIFT_TEST_MARGIN = 15,
  // Every this-many-th move, and every exchange, is also entered as text.
  POOL_SHIFT_TEST_VALIDATE_STRIDE = 7,
  // The mover's weight and the opponent's, which must not count, in
  // thousandths.
  POOL_SHIFT_TEST_WEIGHT = 1200,
  POOL_SHIFT_TEST_DECOY_WEIGHT = 700,
  // The candidates whose replies are generated with a best-move carry.
  POOL_SHIFT_TEST_CARRY_CANDIDATES = 8,
  // Inferences run, on moves of at least this many tiles, so that few
  // leaves are left to enumerate.
  POOL_SHIFT_TEST_INFERENCES = 4,
  POOL_SHIFT_TEST_INFERENCE_MIN_TILES = 5,
  POOL_SHIFT_TEST_INFERENCE_LEAVES = 10,
};

// Sets a player's weight in the players' data, builds the table it needs,
// and gives the game's player the result.
static void set_pool_shift_weight(const Config *config, const Game *game,
                                  int player_index, int weight_milli) {
  PlayersData *players_data = config_get_players_data(config);
  players_data_set_pool_shift_weight(players_data, player_index, weight_milli);
  players_data_update_pool_shift(players_data, config_get_ld(config));
  for (int update_idx = 0; update_idx < 2; update_idx++) {
    player_update(players_data, game_get_player(game, update_idx));
  }
}

// The pool shift of keeping leave, computed in floating point from the
// definition: weight * tiles drawn * the sum over the tiles of (the tile's
// share of the unseen tiles less its share of the distribution) * the value
// of the leave with the tile, in millipoints.
static double get_exact_shift(const KLV *klv, const LetterDistribution *ld,
                              const Bag *bag, const Rack *opponent_rack,
                              const Rack *leave, int weight_milli) {
  const int ld_size = ld_get_size(ld);
  const int unseen_total =
      bag_get_letters(bag) + rack_get_total_letters(opponent_rack);
  Rack extended;
  rack_copy(&extended, leave);
  double sum = 0.0;
  for (int ml = 0; ml < ld_size; ml++) {
    if (rack_get_letter(leave, (MachineLetter)ml) >=
        ld_get_dist(ld, (MachineLetter)ml)) {
      continue;
    }
    const int unseen = bag_get_letter(bag, (MachineLetter)ml) +
                       rack_get_letter(opponent_rack, (MachineLetter)ml);
    rack_add_letter(&extended, (MachineLetter)ml);
    sum += ((double)unseen / (double)unseen_total -
            (double)ld_get_dist(ld, (MachineLetter)ml) /
                (double)ld_get_total_tiles(ld)) *
           (double)klv_get_leave_value(klv, &extended);
    rack_take_letter(&extended, (MachineLetter)ml);
  }
  return (double)weight_milli / 1000.0 *
         (double)(RACK_SIZE - rack_get_total_letters(leave)) * sum;
}

// The first leave_size tiles of rack.
static void set_leave_prefix(const Rack *rack, int leave_size, Rack *leave) {
  rack_set_dist_size_and_reset(leave, rack_get_dist_size(rack));
  for (int ml = 0; ml < rack_get_dist_size(rack); ml++) {
    for (int copy = 0; copy < rack_get_letter(rack, (MachineLetter)ml) &&
                       rack_get_total_letters(leave) < leave_size;
         copy++) {
      rack_add_letter(leave, (MachineLetter)ml);
    }
  }
}

// A table holds the KLV's value of each leave with one more of each tile,
// and the shift it gives is the definition's to within a millipoint. A table
// is of its KLV as it was built: once a leave value changes, the shift no
// longer applies until the table is rebuilt.
static void test_pool_shift_table(void) {
  Config *config = config_create_or_die("set -lex CSW21 -wmp false");
  const LetterDistribution *ld = config_get_ld(config);
  const KLV *klv = players_data_get_klv(config_get_players_data(config), 0);
  PoolShiftTable *table = pool_shift_table_create(klv, ld);
  assert(table->ld_size == ld_get_size(ld));
  assert(table->stride % 4 == 0 && table->stride >= table->ld_size);
  assert(pool_shift_table_matches_klv(table, klv));

  Game *game = config_game_create(config);
  int rows_checked = 0;
  int nonzero_shifts = 0;
  for (int game_idx = 0; game_idx < POOL_SHIFT_TEST_GAMES; game_idx++) {
    game_reset(game);
    game_seed(game, (uint64_t)game_idx + 31);
    draw_starting_racks(game);
    while (!game_over(game)) {
      const int player_index = game_get_player_on_turn_index(game);
      const Rack *rack = player_get_rack(game_get_player(game, player_index));
      const Rack *opponent_rack =
          player_get_rack(game_get_player(game, 1 - player_index));
      const int tiles_on_rack = rack_get_total_letters(rack);
      const int real_bag_size = game_get_real_bag_size(game);
      for (int leave_size = 0; leave_size <= tiles_on_rack; leave_size++) {
        Rack leave;
        set_leave_prefix(rack, leave_size, &leave);
        const uint32_t row = pool_shift_table_get_leave_row(table, klv, &leave);
        const Equity shift = pool_shift_get_leave_shift(
            table, klv, POOL_SHIFT_TEST_WEIGHT, ld, game_get_bag(game),
            opponent_rack, &leave, tiles_on_rack, real_bag_size);
        if (leave_size > POOL_SHIFT_MAX_LEAVE_SIZE) {
          assert(row == POOL_SHIFT_NO_ROW);
          assert(shift == 0);
          continue;
        }
        assert(row != POOL_SHIFT_NO_ROW);
        assert((row == POOL_SHIFT_EMPTY_LEAVE_ROW) == (leave_size == 0));
        Rack extended;
        rack_copy(&extended, &leave);
        for (int ml = 0; ml < table->stride; ml++) {
          Equity expected = 0;
          if (ml < ld_get_size(ld) &&
              rack_get_letter(&leave, (MachineLetter)ml) <
                  ld_get_dist(ld, (MachineLetter)ml)) {
            rack_add_letter(&extended, (MachineLetter)ml);
            expected = klv_get_leave_value(klv, &extended);
            rack_take_letter(&extended, (MachineLetter)ml);
          }
          assert(
              table->values[(size_t)row * (size_t)table->stride + (size_t)ml] ==
              expected);
        }
        rows_checked++;
        if (tiles_on_rack != RACK_SIZE || real_bag_size < RACK_SIZE) {
          assert(shift == 0);
          continue;
        }
        const double exact =
            get_exact_shift(klv, ld, game_get_bag(game), opponent_rack, &leave,
                            POOL_SHIFT_TEST_WEIGHT);
        assert(fabs((double)shift - exact) <= 1.0);
        nonzero_shifts += shift != 0;
        // The weight scales the shift, and none gives none.
        assert(pool_shift_get_leave_shift(table, klv, 0, ld, game_get_bag(game),
                                          opponent_rack, &leave, tiles_on_rack,
                                          real_bag_size) == 0);
      }
      play_top_n_equity_move(game, 0);
    }
  }
  assert(rows_checked > 100);
  assert(nonzero_shifts > 100);

  // Nothing unseen is in the distribution's proportions before the first
  // draw, so every shift is 0.
  game_reset(game);
  int32_t weights[POOL_SHIFT_MAX_STRIDE];
  pool_shift_fill_weights(table, ld, game_get_bag(game),
                          player_get_rack(game_get_player(game, 1)),
                          POOL_SHIFT_TEST_WEIGHT, weights);
  for (int ml = 0; ml < table->stride; ml++) {
    assert(weights[ml] == 0);
  }

  // A changed leave value makes the table another KLV's.
  Rack single;
  rack_set_dist_size_and_reset(&single, ld_get_size(ld));
  rack_add_letter(&single, 1);
  const uint32_t single_index = klv_get_word_index(klv, &single);
  const Equity single_value = klv_get_indexed_leave_value(klv, single_index);
  klv_set_indexed_leave_value(klv, single_index, single_value + 1000);
  assert(!pool_shift_table_matches_klv(table, klv));
  assert(!pool_shift_applies(table, klv, POOL_SHIFT_TEST_WEIGHT, RACK_SIZE,
                             RACK_SIZE));
  PoolShiftTable *rebuilt = pool_shift_table_create(klv, ld);
  assert(pool_shift_applies(rebuilt, klv, POOL_SHIFT_TEST_WEIGHT, RACK_SIZE,
                            RACK_SIZE));
  assert(rebuilt->values[(size_t)POOL_SHIFT_EMPTY_LEAVE_ROW + 1] ==
         single_value + 1000);
  klv_set_indexed_leave_value(klv, single_index, single_value);

  pool_shift_table_destroy(rebuilt);
  pool_shift_table_destroy(table);
  game_destroy(game);
  config_destroy(config);
}

static bool settings_have(const Config *config, const char *setting,
                          bool saved_only) {
  StringBuilder *settings = string_builder_create();
  if (saved_only) {
    config_add_saved_settings_to_string_builder(config, settings);
  } else {
    config_add_settings_to_string_builder(config, settings);
  }
  const bool has = strstr(string_builder_peek(settings), setting) != NULL;
  string_builder_destroy(settings);
  return has;
}

// The players' weights and tables are as the options leave them, and the
// live game's players have them.
static void assert_config_pool_shift(const Config *config, int first_weight,
                                     int second_weight) {
  const PlayersData *players_data = config_get_players_data(config);
  const int weights[2] = {first_weight, second_weight};
  for (int player_index = 0; player_index < 2; player_index++) {
    assert(players_data_get_pool_shift_weight(players_data, player_index) ==
           weights[player_index]);
    const PoolShiftTable *table =
        players_data_get_pool_shift_table(players_data, player_index);
    assert((table != NULL) == (weights[player_index] != 0));
    if (table != NULL) {
      assert(pool_shift_table_matches_klv(
          table, players_data_get_klv(players_data, player_index)));
    }
    const Player *player =
        game_get_player(config_get_game(config), player_index);
    assert(player_get_pool_shift_table(player) == table);
    assert(player_get_pool_shift_weight(player) == weights[player_index]);
  }
}

static void test_pool_shift_config(void) {
  Config *config = config_create_or_die("set -lex CSW21 -wmp false");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  const PlayersData *players_data = config_get_players_data(config);
  // Off by default, and then nothing is built or written.
  assert_config_pool_shift(config, 0, 0);
  assert(!settings_have(config, "poolshift", false));

  load_and_exec_config_or_die(config, "set -poolshift 1.2");
  assert_config_pool_shift(config, 1200, 1200);
  // Players with the same leaves share a table.
  assert(players_data_get_pool_shift_table(players_data, 0) ==
         players_data_get_pool_shift_table(players_data, 1));
  assert(settings_have(config, "-poolshift1 1.200", false));
  assert(settings_have(config, "-poolshift2 1.200", true));

  // A player's own option overrides the one for both, and an unset option
  // keeps the weight.
  load_and_exec_config_or_die(config, "set -poolshift 0.5 -poolshift1 0");
  assert_config_pool_shift(config, 0, 500);
  assert(!settings_have(config, "-poolshift1", false));
  assert(settings_have(config, "-poolshift2 0.500", false));
  load_and_exec_config_or_die(config, "set -numplays 3");
  assert_config_pool_shift(config, 0, 500);
  load_and_exec_config_or_die(config, "set -poolshift1 0.75");
  assert_config_pool_shift(config, 750, 500);

  // The tables follow the leaves.
  load_and_exec_config_or_die(config, "set -lex NWL20");
  assert_config_pool_shift(config, 750, 500);
  load_and_exec_config_or_die(config, "set -lex CSW21 -k2 NWL20");
  assert_config_pool_shift(config, 750, 500);
  assert(players_data_get_pool_shift_table(players_data, 0) !=
         players_data_get_pool_shift_table(players_data, 1));
  load_and_exec_config_or_die(config, "set -leaves CSW21");
  assert_config_pool_shift(config, 750, 500);
  assert(players_data_get_pool_shift_table(players_data, 0) ==
         players_data_get_pool_shift_table(players_data, 1));

  // A weight that is not one leaves the weights as they were.
  assert_config_exec_status(config, "set -poolshift -1",
                            ERROR_STATUS_CONFIG_LOAD_DOUBLE_ARG_OUT_OF_BOUNDS);
  assert_config_exec_status(config, "set -poolshift2 10.5",
                            ERROR_STATUS_CONFIG_LOAD_DOUBLE_ARG_OUT_OF_BOUNDS);
  assert_config_exec_status(config, "set -poolshift1 much",
                            ERROR_STATUS_CONFIG_LOAD_MALFORMED_DOUBLE_ARG);
  assert_config_pool_shift(config, 750, 500);

  load_and_exec_config_or_die(config, "set -poolshift 0");
  assert_config_pool_shift(config, 0, 0);
  config_destroy(config);
}

static MoveList *generate_moves_recorded(const Game *game, int capacity,
                                         move_record_t move_record_type,
                                         Equity eq_margin_movegen) {
  MoveList *move_list = move_list_create(capacity);
  const MoveGenArgs args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = move_record_type,
      .move_sort_type = MOVE_SORT_EQUITY,
      .eq_margin_movegen = eq_margin_movegen,
      .target_equity = EQUITY_MAX_VALUE,
  };
  generate_moves(&args);
  return move_list;
}

// Every move, in a list grown until it holds them all.
static MoveList *generate_all_moves(const Game *game) {
  int capacity = POOL_SHIFT_TEST_MOVE_LIST_CAPACITY;
  while (true) {
    MoveList *move_list =
        generate_moves_recorded(game, capacity, MOVE_RECORD_ALL, 0);
    if (move_list_get_count(move_list) < capacity) {
      return move_list;
    }
    move_list_destroy(move_list);
    capacity *= 2;
  }
}

static Equity generate_best_move_equity(const Game *game) {
  MoveList *move_list = generate_moves_recorded(game, 1, MOVE_RECORD_BEST, 0);
  const Equity equity = move_get_equity(move_list_get_move(move_list, 0));
  move_list_destroy(move_list);
  return equity;
}

static int compare_equities_descending(const void *first, const void *second) {
  const Equity first_equity = *(const Equity *)first;
  const Equity second_equity = *(const Equity *)second;
  if (first_equity != second_equity) {
    return first_equity > second_equity ? -1 : 1;
  }
  return 0;
}

// The equities of a move list, highest first. The caller frees them.
static Equity *get_sorted_equities(const MoveList *move_list) {
  const int count = move_list_get_count(move_list);
  Equity *equities = malloc_or_die(sizeof(Equity) * ((size_t)count + 1));
  for (int move_idx = 0; move_idx < count; move_idx++) {
    equities[move_idx] =
        move_get_equity(move_list_get_move(move_list, move_idx));
  }
  qsort(equities, (size_t)count, sizeof(Equity), compare_equities_descending);
  return equities;
}

// The equity the mover's validation of move's text gives it.
static Equity get_validated_equity(const Game *game, const Move *move) {
  StringBuilder *move_string = string_builder_create();
  string_builder_add_ucgi_move(move_string, move, game_get_board(game),
                               game_get_ld(game));
  ErrorStack *error_stack = error_stack_create();
  ValidatedMoves *validated_moves = validated_moves_create(
      game, game_get_player_on_turn_index(game),
      string_builder_peek(move_string), true, true, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(validated_moves_get_number_of_moves(validated_moves) == 1);
  const Equity equity =
      move_get_equity(validated_moves_get_move(validated_moves, 0));
  validated_moves_destroy(validated_moves);
  error_stack_destroy(error_stack);
  string_builder_destroy(move_string);
  return equity;
}

// The shift the mover's weight gives move in game, from the table and the
// position, which the caller reads before the weights change.
static Equity get_expected_shift(const Game *game, const PoolShiftTable *table,
                                 int weight_milli, const Move *move) {
  if (move_get_type(move) == GAME_EVENT_PASS) {
    return 0;
  }
  const int player_index = game_get_player_on_turn_index(game);
  const Player *player = game_get_player(game, player_index);
  Rack leave;
  if (move_get_type(move) == GAME_EVENT_EXCHANGE) {
    // An exchanged blank is the blank's letter, which a play's tiles use to
    // mark the board's tiles.
    rack_copy(&leave, player_get_rack(player));
    for (int tile_idx = 0; tile_idx < move_get_tiles_length(move); tile_idx++) {
      rack_take_letter(&leave, move_get_tile(move, tile_idx));
    }
  } else {
    get_leave_for_move(move, game, &leave);
  }
  return pool_shift_get_leave_shift(
      table, player_get_klv(player), weight_milli, game_get_ld(game),
      game_get_bag(game),
      player_get_rack(game_get_player(game, 1 - player_index)), &leave,
      rack_get_total_letters(player_get_rack(player)),
      game_get_real_bag_size(game));
}

// With the mover's weight (and another for the opponent, which must not
// count):
// - every play's and exchange's equity is its equity without the pool shift
//   plus the shift of its leave, and a pass's is unchanged;
// - the best move recorded is the best of all moves, which checks that the
//   shadow, WMP and rack info table bounds still bound every move, and that
//   the best exchange is found among shifted leaves;
// - a top-n recording holds the n best equities of all moves;
// - a within-x-of-best recording holds every move within x of the best;
// - entering a move as text adds the same shift;
// - generations without the shift, made between ones with it, find what they
//   found before, and so do the ones with it: what one kind remembers of a
//   rack must not reach the other.
// Returns whether any move's equity was shifted.
static bool assert_pool_shift_equities(const Config *config, const Game *game) {
  const int player_index = game_get_player_on_turn_index(game);
  set_pool_shift_weight(config, game, 1 - player_index,
                        POOL_SHIFT_TEST_DECOY_WEIGHT);

  set_pool_shift_weight(config, game, player_index, 0);
  MoveList *without = generate_all_moves(game);
  const Equity best_without = generate_best_move_equity(game);

  set_pool_shift_weight(config, game, player_index, POOL_SHIFT_TEST_WEIGHT);
  const PoolShiftTable *table =
      player_get_pool_shift_table(game_get_player(game, player_index));
  assert(table != NULL);
  MoveList *with = generate_all_moves(game);
  const Equity best_equity = generate_best_move_equity(game);

  set_pool_shift_weight(config, game, player_index, 0);
  assert(generate_best_move_equity(game) == best_without);
  set_pool_shift_weight(config, game, player_index, POOL_SHIFT_TEST_WEIGHT);
  assert(generate_best_move_equity(game) == best_equity);

  bool any_shifted = false;
  const int count = move_list_get_count(without);
  assert(count == move_list_get_count(with));
  for (int with_idx = 0; with_idx < count; with_idx++) {
    const Move *with_move = move_list_get_move(with, with_idx);
    const Move *without_move = NULL;
    for (int without_idx = 0; without_idx < count; without_idx++) {
      const Move *candidate = move_list_get_move(without, without_idx);
      if (compare_moves_without_equity(with_move, candidate, true) == -1) {
        without_move = candidate;
        break;
      }
    }
    assert(without_move != NULL);
    const Equity shift =
        get_expected_shift(game, table, POOL_SHIFT_TEST_WEIGHT, with_move);
    assert(move_get_equity(with_move) == move_get_equity(without_move) + shift);
    any_shifted = any_shifted || shift != 0;
  }

  Equity *all_equities = get_sorted_equities(with);
  assert(best_equity == all_equities[0]);

  MoveList *top =
      generate_moves_recorded(game, POOL_SHIFT_TEST_TOP_N, MOVE_RECORD_ALL, 0);
  const int top_count = move_list_get_count(top);
  assert(top_count ==
         (count < POOL_SHIFT_TEST_TOP_N ? count : POOL_SHIFT_TEST_TOP_N));
  Equity *top_equities = get_sorted_equities(top);
  for (int top_idx = 0; top_idx < top_count; top_idx++) {
    assert(top_equities[top_idx] == all_equities[top_idx]);
  }

  const Equity margin = int_to_equity(POOL_SHIFT_TEST_MARGIN);
  MoveList *within =
      generate_moves_recorded(game, POOL_SHIFT_TEST_MOVE_LIST_CAPACITY,
                              MOVE_RECORD_WITHIN_X_EQUITY_OF_BEST, margin);
  int expected_within = 0;
  while (expected_within < count &&
         all_equities[expected_within] >= all_equities[0] - margin) {
    expected_within++;
  }
  assert(move_list_get_count(within) == expected_within);
  Equity *within_equities = get_sorted_equities(within);
  for (int within_idx = 0; within_idx < expected_within; within_idx++) {
    assert(within_equities[within_idx] == all_equities[within_idx]);
  }

  for (int with_idx = 0; with_idx < count; with_idx++) {
    const Move *move = move_list_get_move(with, with_idx);
    const game_event_t type = move_get_type(move);
    if ((type == GAME_EVENT_EXCHANGE &&
         with_idx % POOL_SHIFT_TEST_VALIDATE_STRIDE == 0) ||
        (type == GAME_EVENT_TILE_PLACEMENT_MOVE &&
         with_idx % POOL_SHIFT_TEST_VALIDATE_STRIDE == 0)) {
      assert(get_validated_equity(game, move) == move_get_equity(move));
    }
  }

  // The opponent's rack is as unseen in the bag as on its rack: with it not
  // known, the real bag and the pool are the same, and so is every equity.
  if (game_get_real_bag_size(game) > 0) {
    Game *unknown = game_duplicate(game);
    return_rack_to_bag(unknown, 1 - player_index);
    assert(game_get_real_bag_size(unknown) == game_get_real_bag_size(game));
    MoveList *unknown_moves = generate_all_moves(unknown);
    assert(move_list_get_count(unknown_moves) == count);
    Equity *unknown_equities = get_sorted_equities(unknown_moves);
    for (int move_idx = 0; move_idx < count; move_idx++) {
      assert(unknown_equities[move_idx] == all_equities[move_idx]);
    }
    assert(generate_best_move_equity(unknown) == best_equity);
    free(unknown_equities);
    move_list_destroy(unknown_moves);
    game_destroy(unknown);
  }

  free(within_equities);
  move_list_destroy(within);
  free(top_equities);
  move_list_destroy(top);
  free(all_equities);
  move_list_destroy(without);
  move_list_destroy(with);
  return any_shifted;
}

static void test_pool_shift_movegen(const char *set_cmd) {
  Config *config = config_create_or_die(set_cmd);
  Game *game = config_game_create(config);
  int shifted_positions = 0;
  int unshifted_positions = 0;
  for (int game_idx = 0; game_idx < POOL_SHIFT_TEST_GAMES; game_idx++) {
    game_reset(game);
    game_seed(game, (uint64_t)game_idx + 1);
    draw_starting_racks(game);
    while (!game_over(game)) {
      const bool shifted = assert_pool_shift_equities(config, game);
      // The shift is for full racks drawing from a bag of at least a rack.
      const Rack *rack = player_get_rack(
          game_get_player(game, game_get_player_on_turn_index(game)));
      if (rack_get_total_letters(rack) != RACK_SIZE ||
          game_get_real_bag_size(game) < RACK_SIZE) {
        assert(!shifted);
      }
      shifted_positions += shifted;
      unshifted_positions += !shifted;
      play_top_n_equity_move(game, 0);
    }
  }
  assert(shifted_positions > 10 * POOL_SHIFT_TEST_GAMES);
  assert(unshifted_positions >= POOL_SHIFT_TEST_GAMES);
  set_pool_shift_weight(config, game, 0, 0);
  set_pool_shift_weight(config, game, 1, 0);
  game_destroy(game);
  config_destroy(config);
}

// The best move with the carry, which starts from the moves earlier
// generations remembered, is the best move without it. With the pool shift a
// move's equity depends on the pool, which differs from one candidate's
// replies to the next's while the replier's rack is the same.
static void test_pool_shift_carry(const char *set_cmd) {
  Config *config = config_create_or_die(set_cmd);
  Game *game = config_game_create(config);
  set_pool_shift_weight(config, game, 0, POOL_SHIFT_TEST_WEIGHT);
  set_pool_shift_weight(config, game, 1, POOL_SHIFT_TEST_WEIGHT);
  MoveList *candidates = move_list_create(POOL_SHIFT_TEST_CARRY_CANDIDATES);
  MoveList *plain_list = move_list_create(1);
  MoveList *carried_list = move_list_create(1);
  BestMoveCarry *carry = best_move_carry_create(1024, true);
  int generations = 0;
  for (int game_idx = 0; game_idx < POOL_SHIFT_TEST_GAMES; game_idx++) {
    game_reset(game);
    game_seed(game, (uint64_t)game_idx + 500);
    draw_starting_racks(game);
    while (!game_over(game)) {
      if (bag_get_letters(game_get_bag(game)) > 0) {
        best_move_carry_reset(carry);
        const MoveGenArgs candidate_args = {
            .game = game,
            .move_list = candidates,
            .move_record_type = MOVE_RECORD_ALL,
            .move_sort_type = MOVE_SORT_EQUITY,
            .eq_margin_movegen = 0,
            .target_equity = EQUITY_MAX_VALUE,
        };
        generate_moves(&candidate_args);
        for (int candidate_idx = 0;
             candidate_idx < move_list_get_count(candidates); candidate_idx++) {
          Game *playout = game_duplicate(game);
          play_move(move_list_get_move(candidates, candidate_idx), playout,
                    NULL);
          for (int ply = 0; ply < 2 && !game_over(playout) &&
                            bag_get_letters(game_get_bag(playout)) > 0;
               ply++) {
            const Move *plain = get_top_equity_move(playout, plain_list);
            const Move *carried =
                get_top_equity_move_with_carry(playout, carried_list, carry);
            assert(compare_moves_without_equity(plain, carried, true) == -1);
            assert(move_get_equity(plain) == move_get_equity(carried));
            play_move(plain, playout, NULL);
            generations++;
          }
          game_destroy(playout);
        }
      }
      play_move(get_top_equity_move(game, plain_list), game, NULL);
    }
  }
  assert(generations > 100);
  best_move_carry_destroy(carry);
  move_list_destroy(carried_list);
  move_list_destroy(plain_list);
  move_list_destroy(candidates);
  set_pool_shift_weight(config, game, 0, 0);
  set_pool_shift_weight(config, game, 1, 0);
  game_destroy(game);
  config_destroy(config);
}

// The number and mean value of the leaves inference finds consistent with
// the player on turn making move.
static void get_inferred_leaves(const Config *config, const Game *game,
                                const Move *move, uint64_t *samples,
                                double *mean) {
  const int ld_size = ld_get_size(game_get_ld(game));
  Rack played_tiles;
  rack_set_dist_size_and_reset(&played_tiles, ld_size);
  for (int tile_idx = 0; tile_idx < move_get_tiles_length(move); tile_idx++) {
    const MachineLetter ml = move_get_tile(move, tile_idx);
    if (ml != PLAYED_THROUGH_MARKER) {
      rack_add_letter(&played_tiles,
                      get_is_blanked(ml) ? BLANK_MACHINE_LETTER : ml);
    }
  }
  Rack known_rack;
  rack_set_dist_size_and_reset(&known_rack, ld_size);
  Rack nontarget_known_rack;
  rack_set_dist_size_and_reset(&nontarget_known_rack, ld_size);
  ThreadControl *thread_control = config_get_thread_control(config);
  thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED);
  InferenceArgs args;
  infer_args_fill(&args, POOL_SHIFT_TEST_INFERENCE_LEAVES, 0, NULL, game, 1, 0,
                  0, thread_control, false, true,
                  game_get_player_on_turn_index(game), move_get_score(move), 0,
                  &played_tiles, &known_rack, &nontarget_known_rack);
  InferenceResults *results = inference_results_create(NULL);
  ErrorStack *error_stack = error_stack_create();
  infer_without_ctx(&args, results, error_stack);
  assert(error_stack_is_empty(error_stack));
  const Stat *leaves =
      inference_results_get_equity_values(results, INFERENCE_TYPE_LEAVE);
  *samples = stat_get_num_samples(leaves);
  *mean = stat_get_mean(leaves);
  error_stack_destroy(error_stack);
  inference_results_destroy(results);
}

// Inference's game holds every tile the target cannot see in the bag, so the
// pool it would shift by is not the one the target played from, and the
// equities it computes itself have no shift. It leaves the pool shift out:
// the leaves it infers are the same with a weight set.
static void test_pool_shift_inference(const char *set_cmd) {
  Config *config = config_create_or_die(set_cmd);
  Game *game = config_game_create(config);
  MoveList *best_list = move_list_create(1);
  int inferences = 0;
  for (int game_idx = 0; game_idx < 8 * POOL_SHIFT_TEST_GAMES &&
                         inferences < POOL_SHIFT_TEST_INFERENCES;
       game_idx++) {
    game_reset(game);
    game_seed(game, (uint64_t)game_idx + 3000);
    draw_starting_racks(game);
    while (!game_over(game)) {
      const Move *best = get_top_equity_move(game, best_list);
      if (game_get_real_bag_size(game) >= RACK_SIZE &&
          inferences < POOL_SHIFT_TEST_INFERENCES &&
          move_get_type(best) == GAME_EVENT_TILE_PLACEMENT_MOVE &&
          move_get_tiles_played(best) >= POOL_SHIFT_TEST_INFERENCE_MIN_TILES) {
        uint64_t samples_without = 0;
        double mean_without = 0;
        get_inferred_leaves(config, game, best, &samples_without,
                            &mean_without);
        Move played;
        move_copy(&played, best);
        for (int player_index = 0; player_index < 2; player_index++) {
          set_pool_shift_weight(config, game, player_index,
                                POOL_SHIFT_TEST_WEIGHT);
        }
        uint64_t samples_with = 0;
        double mean_with = 0;
        get_inferred_leaves(config, game, &played, &samples_with, &mean_with);
        for (int player_index = 0; player_index < 2; player_index++) {
          set_pool_shift_weight(config, game, player_index, 0);
        }
        assert(samples_with == samples_without);
        assert(mean_with == mean_without);
        inferences++;
      }
      play_move(get_top_equity_move(game, best_list), game, NULL);
    }
  }
  assert(inferences == POOL_SHIFT_TEST_INFERENCES);
  move_list_destroy(best_list);
  game_destroy(game);
  config_destroy(config);
}

void test_pool_shift(void) {
  test_pool_shift_table();
  test_pool_shift_config();
  // The word map has its own leave bounds and its own cache of a rack's
  // subracks.
  test_pool_shift_movegen("set -lex CSW21 -wmp false -s1 equity -s2 equity "
                          "-r1 all -r2 all -numplays 1");
  test_pool_shift_movegen("set -lex CSW21 -wmp true -s1 equity -s2 equity "
                          "-r1 all -r2 all -numplays 1");
  test_pool_shift_carry("set -lex CSW21 -wmp true -s1 equity -s2 equity "
                        "-r1 best -r2 best -numplays 1");
  test_pool_shift_inference("set -lex CSW21 -wmp true -s1 equity -s2 equity "
                            "-r1 all -r2 all -numplays 1");
  // The rack info table has leave values, best leaves and a best exchange of
  // its own, all without the shift. It is in the data directory where make
  // prepare_data has built it, for the standard board. (DEFAULT_BOARD_DIM is
  // an enum constant, which a preprocessor #if reads as 0.)
  // NOLINTNEXTLINE(misc-redundant-expression)
  const bool is_standard_board = BOARD_DIM == DEFAULT_BOARD_DIM;
  const bool have_tables =
      is_standard_board &&
      cfile_access("./data/lexica/CSW24.rit", CFILE_ACCESS_READ) &&
      cfile_access("./data/lexica/CSW24.wit", CFILE_ACCESS_READ);
  if (have_tables) {
    test_pool_shift_movegen(
        "set -lex CSW24 -wmp true -rit true -wit true -s1 equity -s2 equity "
        "-r1 all -r2 all -numplays 1");
    test_pool_shift_carry(
        "set -lex CSW24 -wmp true -rit true -wit true -s1 equity -s2 equity "
        "-r1 best -r2 best -numplays 1");
  }
}
