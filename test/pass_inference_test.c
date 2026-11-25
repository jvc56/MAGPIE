#include "pass_inference_test.h"

#include "../src/def/bit_rack_defs.h"
#include "../src/def/equity_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/ent/bit_rack.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/klv.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/rack_value_table.h"
#include "../src/impl/census.h"
#include "../src/impl/census_play_scores.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/pass_inference.h"
#include "../src/util/io_util.h"
#include "test_constants.h"
#include "test_util.h"
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
  // one rack in this many has no value in the test's tables
  PASS_INFERENCE_TEST_UNVALUED_PERIOD = 11,
  // the test's values run from 0 to this many thousandths of a point
  PASS_INFERENCE_TEST_VALUE_RANGE = 80001,
  PASS_INFERENCE_TEST_THREADS = 4,
};

#define PASS_INFERENCE_TEST_TOLERANCE 1e-12

static bool pass_inference_test_is_valued(uint32_t rack_index) {
  return rack_index % PASS_INFERENCE_TEST_UNVALUED_PERIOD !=
         PASS_INFERENCE_TEST_UNVALUED_PERIOD - 1;
}

static Equity pass_inference_test_value(uint32_t rack_index) {
  return (Equity)(rack_index * 7919U % PASS_INFERENCE_TEST_VALUE_RANGE);
}

// A table of the game's distribution with made-up values, for leaves of the
// given hash.
static RackValueTable *pass_inference_test_create_table(const Game *game,
                                                        uint64_t klv_hash) {
  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);
  uint8_t letter_counts[BIT_RACK_MAX_ALPHABET_SIZE] = {0};
  for (int ml = 0; ml < ld_size; ml++) {
    letter_counts[ml] = (uint8_t)ld_get_dist(ld, (MachineLetter)ml);
  }
  RackValueTable *rvt =
      rack_value_table_create_unvalued(letter_counts, ld_size, klv_hash);
  assert(rvt != NULL);
  const uint32_t number_of_racks = rack_value_table_get_number_of_racks(rvt);
  for (uint32_t rack_index = 0; rack_index < number_of_racks; rack_index++) {
    if (pass_inference_test_is_valued(rack_index)) {
      rack_value_table_write_stored(rvt, rack_index,
                                    pass_inference_test_value(rack_index));
    }
  }
  return rvt;
}

static uint64_t pass_inference_test_passer_klv_hash(const Game *game) {
  return klv_get_hash(player_get_klv(
      game_get_player(game, game_get_player_on_turn_index(game))));
}

static double pass_inference_test_relative_difference(double actual,
                                                      double expected) {
  return fabs(actual - expected) / fabs(expected);
}

// The walk of every rack the passer could hold, for the check of a pass
// inference against the move generator.
typedef struct PassInferenceTestWalk {
  const PassInference *pass_inference;
  const RackValueTable *rvt;
  // a copy of the game, whose passer's rack the walk sets
  Game *game;
  MoveList *move_list;
  int ld_size;
  double temperature;
  double bonus;
  // the move generator values one valued rack in this many
  uint32_t movegen_period;
  // the tiles the observer has not seen
  uint8_t pool[CENSUS_MAX_LETTERS];
  uint8_t tally[CENSUS_MAX_LETTERS];
  uint32_t racks;
  uint32_t valued_racks;
  uint32_t movegen_racks;
  double total_prior;
  double movegen_mass;
} PassInferenceTestWalk;

static uint64_t pass_inference_test_choose(int total, int taken) {
  uint64_t ways = 1;
  for (int taken_idx = 0; taken_idx < taken; taken_idx++) {
    ways = ways * (uint64_t)(total - taken_idx) / (uint64_t)(taken_idx + 1);
  }
  return ways;
}

// The static equity of the best move of the walk's rack, from the move
// generator, as an inference of a play values a rack.
static Equity pass_inference_test_movegen_best(PassInferenceTestWalk *walk,
                                               const Rack *rack) {
  Rack *passer_rack = player_get_rack(
      game_get_player(walk->game, game_get_player_on_turn_index(walk->game)));
  rack_copy(passer_rack, rack);
  return move_get_equity(get_top_equity_move_for_inferences(
      walk->game, walk->move_list, EQUITY_MAX_VALUE, EQUITY_MAX_VALUE));
}

static void pass_inference_test_check_rack(PassInferenceTestWalk *walk) {
  const PassInference *pass_inference = walk->pass_inference;
  Rack rack;
  rack_set_dist_size_and_reset(&rack, walk->ld_size);
  BitRack bit_rack = bit_rack_create_empty();
  uint64_t ways = 1;
  for (int ml = 0; ml < walk->ld_size; ml++) {
    for (int copy_idx = 0; copy_idx < walk->tally[ml]; copy_idx++) {
      rack_add_letter(&rack, (MachineLetter)ml);
      bit_rack_add_letter(&bit_rack, (MachineLetter)ml);
    }
    ways *= pass_inference_test_choose(walk->pool[ml], walk->tally[ml]);
  }
  walk->racks++;
  const uint32_t found_idx = pass_inference_find_rack(pass_inference, &rack);
  Equity value = 0;
  if (!rack_value_table_lookup(walk->rvt, &bit_rack, &value)) {
    assert(found_idx == UINT32_MAX);
    return;
  }
  // the racks are numbered in the order of this walk
  assert(found_idx == walk->valued_racks);
  walk->valued_racks++;
  Rack found_rack;
  pass_inference_get_rack(pass_inference, found_idx, &found_rack);
  assert(racks_are_equal(&rack, &found_rack));
  assert(pass_inference_get_value(pass_inference, found_idx) == value);
  const double prior = (double)ways;
  assert(pass_inference_test_relative_difference(
             pass_inference_get_prior(pass_inference, found_idx), prior) <=
         PASS_INFERENCE_TEST_TOLERANCE);
  walk->total_prior += prior;
  if (found_idx % walk->movegen_period != 0) {
    return;
  }
  const Equity best_equity = pass_inference_test_movegen_best(walk, &rack);
  assert(pass_inference_get_best_equity(pass_inference, found_idx) ==
         best_equity);
  const double mass =
      prior / (1.0 + exp((equity_to_double(best_equity) -
                          equity_to_double(value) - walk->bonus) /
                         walk->temperature));
  assert(mass > 0.0);
  assert(pass_inference_test_relative_difference(
             pass_inference_get_mass(pass_inference, found_idx), mass) <=
         PASS_INFERENCE_TEST_TOLERANCE);
  walk->movegen_racks++;
  walk->movegen_mass += mass;
}

// Every rack of `remaining` more tiles from the letters ml and after.
static void pass_inference_test_walk_racks(PassInferenceTestWalk *walk, int ml,
                                           int remaining) {
  if (remaining == 0) {
    pass_inference_test_check_rack(walk);
    return;
  }
  if (ml == walk->ld_size) {
    return;
  }
  for (int count = 0; count <= walk->pool[ml] && count <= remaining; count++) {
    walk->tally[ml] = (uint8_t)count;
    pass_inference_test_walk_racks(walk, ml + 1, remaining - count);
  }
  walk->tally[ml] = 0;
}

static PassInference *pass_inference_test_infer(const Game *game,
                                                const RackValueTable *rvt,
                                                double temperature,
                                                double bonus, int num_threads) {
  const PassInferenceArgs args = {
      .game = game,
      .rack_value_table = rvt,
      .temperature = temperature,
      .bonus = bonus,
      .num_threads = num_threads,
  };
  ErrorStack *error_stack = error_stack_create();
  PassInference *pass_inference = pass_inference_create(&args, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(pass_inference != NULL);
  error_stack_destroy(error_stack);
  return pass_inference;
}

// A pass inference of the position against the move generator: every rack
// the passer could hold, its value, prior, best equity and mass, the move
// generator valuing one rack in movegen_period. Returns the number of racks.
static uint32_t pass_inference_test_against_movegen(
    Config *config, const char *cgp_command, double temperature, double bonus,
    uint32_t movegen_period, uint32_t min_racks) {
  load_and_exec_config_or_die(config, cgp_command);
  const Game *game = config_get_game(config);
  RackValueTable *rvt = pass_inference_test_create_table(
      game, pass_inference_test_passer_klv_hash(game));
  PassInference *pass_inference = pass_inference_test_infer(
      game, rvt, temperature, bonus, PASS_INFERENCE_TEST_THREADS);

  PassInferenceTestWalk walk = {
      .pass_inference = pass_inference,
      .rvt = rvt,
      .game = game_duplicate(game),
      .move_list = move_list_create(1),
      .ld_size = ld_get_size(game_get_ld(game)),
      .temperature = temperature,
      .bonus = bonus,
      .movegen_period = movegen_period,
  };
  // The move generator must value the racks as the inference does: with no
  // pre-endgame adjustment.
  const int passer_index = game_get_player_on_turn_index(game);
  for (int player_index = 0; player_index < 2; player_index++) {
    player_set_peg_adjust(game_get_player(walk.game, player_index), NULL);
  }
  // The tiles the observer has not seen, found another way than the
  // inference finds them: everything off the board but their own rack.
  census_get_unseen_tally(game, walk.pool);
  const Rack *observer_rack =
      player_get_rack(game_get_player(game, 1 - passer_index));
  for (int ml = 0; ml < walk.ld_size; ml++) {
    walk.pool[ml] =
        (uint8_t)(walk.pool[ml] -
                  rack_get_letter(observer_rack, (MachineLetter)ml));
  }
  pass_inference_test_walk_racks(&walk, 0, RACK_SIZE);

  const uint32_t number_of_racks =
      pass_inference_get_number_of_racks(pass_inference);
  assert(number_of_racks == walk.valued_racks);
  assert(number_of_racks >= min_racks);
  // some racks of the walk have no value
  assert(walk.racks > walk.valued_racks);
  assert(walk.movegen_racks ==
         (number_of_racks + movegen_period - 1) / movegen_period);
  assert(pass_inference_test_relative_difference(
             pass_inference_get_total_prior(pass_inference),
             walk.total_prior) <= PASS_INFERENCE_TEST_TOLERANCE);
  double total_mass = 0.0;
  double total_probability = 0.0;
  for (uint32_t rack_idx = 0; rack_idx < number_of_racks; rack_idx++) {
    total_mass += pass_inference_get_mass(pass_inference, rack_idx);
    total_probability +=
        pass_inference_get_probability(pass_inference, rack_idx);
  }
  assert(pass_inference_get_total_mass(pass_inference) == total_mass);
  assert(fabs(total_probability - 1.0) <= 1e-9);
  if (movegen_period == 1) {
    assert(pass_inference_test_relative_difference(total_mass,
                                                   walk.movegen_mass) <= 1e-9);
  }

  // A rack with more of a letter than is unseen, and one short of a tile,
  // are not among the racks.
  Rack undrawable_rack;
  rack_set_dist_size_and_reset(&undrawable_rack, walk.ld_size);
  rack_add_letters(&undrawable_rack, BLANK_MACHINE_LETTER, RACK_SIZE);
  assert(walk.pool[BLANK_MACHINE_LETTER] < RACK_SIZE);
  assert(pass_inference_find_rack(pass_inference, &undrawable_rack) ==
         UINT32_MAX);
  Rack short_rack;
  pass_inference_get_rack(pass_inference, number_of_racks - 1, &short_rack);
  assert(pass_inference_find_rack(pass_inference, &short_rack) ==
         number_of_racks - 1);
  for (int ml = 0; ml < walk.ld_size; ml++) {
    if (rack_get_letter(&short_rack, (MachineLetter)ml) > 0) {
      rack_take_letter(&short_rack, (MachineLetter)ml);
      break;
    }
  }
  assert(pass_inference_find_rack(pass_inference, &short_rack) == UINT32_MAX);

  // One thread finds the same numbers as several.
  PassInference *serial_inference =
      pass_inference_test_infer(game, rvt, temperature, bonus, 1);
  assert(pass_inference_get_number_of_racks(serial_inference) ==
         number_of_racks);
  for (uint32_t rack_idx = 0; rack_idx < number_of_racks; rack_idx++) {
    assert(pass_inference_get_best_equity(serial_inference, rack_idx) ==
           pass_inference_get_best_equity(pass_inference, rack_idx));
    assert(pass_inference_get_mass(serial_inference, rack_idx) ==
           pass_inference_get_mass(pass_inference, rack_idx));
  }
  assert(pass_inference_get_total_mass(serial_inference) == total_mass);
  pass_inference_destroy(serial_inference);

  move_list_destroy(walk.move_list);
  game_destroy(walk.game);
  pass_inference_destroy(pass_inference);
  rack_value_table_destroy(rvt);
  return number_of_racks;
}

static void test_pass_inference_against_movegen(void) {
  Config *config = config_create_or_die("set -lex CSW21 -wmp false");
  // every rack of a late position, at the default temperature and bonus
  pass_inference_test_against_movegen(config, "cgp " VS_FRENTZ_CGP,
                                      PASS_INFERENCE_DEFAULT_TEMPERATURE,
                                      PASS_INFERENCE_DEFAULT_BONUS, 1, 16000);
  // some racks of positions with more tiles unseen, with blanks among them,
  // at other temperatures and bonuses
  pass_inference_test_against_movegen(config, "cgp " JOSH2_CGP, 5.0, 12.5, 499,
                                      800000);
  pass_inference_test_against_movegen(config, "cgp " INCOMPLETE_ELISE_CGP, 0.75,
                                      -3.0, 2003, 990000);
  config_destroy(config);
}

// What the temperature and the bonus do to the masses.
static void test_pass_inference_temperature_and_bonus(void) {
  Config *config = config_create_or_die("set -lex CSW21 -wmp false");
  load_and_exec_config_or_die(config, "cgp " VS_FRENTZ_CGP);
  const Game *game = config_get_game(config);
  RackValueTable *rvt = pass_inference_test_create_table(
      game, pass_inference_test_passer_klv_hash(game));
  PassInference *base_inference =
      pass_inference_test_infer(game, rvt, PASS_INFERENCE_DEFAULT_TEMPERATURE,
                                PASS_INFERENCE_DEFAULT_BONUS, 1);
  PassInference *bonus_inference = pass_inference_test_infer(
      game, rvt, PASS_INFERENCE_DEFAULT_TEMPERATURE, 4.0, 1);
  PassInference *sharp_inference = pass_inference_test_infer(
      game, rvt, 0.5, PASS_INFERENCE_DEFAULT_BONUS, 1);
  PassInference *flat_inference = pass_inference_test_infer(
      game, rvt, 1e12, PASS_INFERENCE_DEFAULT_BONUS, 1);
  const uint32_t number_of_racks =
      pass_inference_get_number_of_racks(base_inference);
  assert(number_of_racks > 0);
  int racks_better_passed = 0;
  int racks_better_played = 0;
  int racks_bonus_helps = 0;
  for (uint32_t rack_idx = 0; rack_idx < number_of_racks; rack_idx++) {
    const double prior = pass_inference_get_prior(base_inference, rack_idx);
    const double base_mass = pass_inference_get_mass(base_inference, rack_idx);
    assert(base_mass > 0.0);
    // (a rack worth far more than its best move passes with a probability
    // that rounds to one)
    assert(base_mass <= prior);
    // a bonus for passing makes no rack less likely to pass
    const double bonus_mass =
        pass_inference_get_mass(bonus_inference, rack_idx);
    assert(bonus_mass >= base_mass);
    racks_bonus_helps += bonus_mass > base_mass ? 1 : 0;
    // A sharper passer is likelier to pass with a rack worth more than its
    // best move and less likely with one worth less.
    const Equity value = pass_inference_get_value(base_inference, rack_idx);
    const Equity best_equity =
        pass_inference_get_best_equity(base_inference, rack_idx);
    const double sharp_mass =
        pass_inference_get_mass(sharp_inference, rack_idx);
    if (value > best_equity) {
      assert(sharp_mass >= base_mass);
      assert(base_mass > prior / 2.0);
      racks_better_passed += sharp_mass > base_mass ? 1 : 0;
    } else if (value < best_equity) {
      assert(sharp_mass < base_mass);
      assert(base_mass < prior / 2.0);
      racks_better_played++;
    }
    // a passer with no preference passes half the time with any rack
    assert(pass_inference_test_relative_difference(
               pass_inference_get_mass(flat_inference, rack_idx),
               prior / 2.0) <= 1e-9);
    assert(pass_inference_test_relative_difference(
               pass_inference_get_probability(flat_inference, rack_idx),
               prior / pass_inference_get_total_prior(base_inference)) <= 1e-9);
  }
  assert(racks_better_passed > 100);
  assert(racks_better_played > 100);
  assert((uint32_t)racks_bonus_helps > number_of_racks / 2);
  assert(pass_inference_get_total_mass(bonus_inference) >
         pass_inference_get_total_mass(base_inference));
  pass_inference_destroy(flat_inference);
  pass_inference_destroy(sharp_inference);
  pass_inference_destroy(bonus_inference);
  pass_inference_destroy(base_inference);
  rack_value_table_destroy(rvt);
  config_destroy(config);
}

static void pass_inference_test_assert_error(const Game *game,
                                             const RackValueTable *rvt,
                                             double temperature,
                                             error_code_t expected_error) {
  const PassInferenceArgs args = {
      .game = game,
      .rack_value_table = rvt,
      .temperature = temperature,
      .bonus = PASS_INFERENCE_DEFAULT_BONUS,
      .num_threads = 1,
  };
  ErrorStack *error_stack = error_stack_create();
  const PassInference *pass_inference =
      pass_inference_create(&args, error_stack);
  assert(pass_inference == NULL);
  assert(error_stack_top(error_stack) == expected_error);
  error_stack_destroy(error_stack);
}

// The positions and tables a pass cannot be inferred from.
static void test_pass_inference_errors(void) {
  Config *config = config_create_or_die("set -lex CSW21 -wmp false");
  load_and_exec_config_or_die(config, "cgp " VS_FRENTZ_CGP);
  const Game *game = config_get_game(config);
  const uint64_t klv_hash = pass_inference_test_passer_klv_hash(game);
  RackValueTable *rvt = pass_inference_test_create_table(game, klv_hash);

  pass_inference_test_assert_error(
      game, rvt, 0.0, ERROR_STATUS_PASS_INFERENCE_UNSUPPORTED_GAME);
  pass_inference_test_assert_error(
      game, rvt, -1.0, ERROR_STATUS_PASS_INFERENCE_UNSUPPORTED_GAME);

  // a table of other leaves
  RackValueTable *other_leaves_rvt =
      pass_inference_test_create_table(game, klv_hash + 1);
  pass_inference_test_assert_error(game, other_leaves_rvt,
                                   PASS_INFERENCE_DEFAULT_TEMPERATURE,
                                   ERROR_STATUS_PASS_INFERENCE_TABLE_MISMATCH);
  rack_value_table_destroy(other_leaves_rvt);

  // a table of another distribution: one tile fewer of the last letter
  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);
  uint8_t letter_counts[BIT_RACK_MAX_ALPHABET_SIZE] = {0};
  for (int ml = 0; ml < ld_size; ml++) {
    letter_counts[ml] = (uint8_t)ld_get_dist(ld, (MachineLetter)ml);
  }
  assert(ld_size > 0);
  letter_counts[ld_size - 1]--;
  RackValueTable *other_distribution_rvt =
      rack_value_table_create_unvalued(letter_counts, ld_size, klv_hash);
  pass_inference_test_assert_error(game, other_distribution_rvt,
                                   PASS_INFERENCE_DEFAULT_TEMPERATURE,
                                   ERROR_STATUS_PASS_INFERENCE_TABLE_MISMATCH);
  rack_value_table_destroy(other_distribution_rvt);

  // an empty board
  load_and_exec_config_or_die(config, "cgp " OPENING_CGP);
  pass_inference_test_assert_error(
      config_get_game(config), rvt, PASS_INFERENCE_DEFAULT_TEMPERATURE,
      ERROR_STATUS_PASS_INFERENCE_UNSUPPORTED_GAME);

  // A game played until the bag is too short for an exchange: the last
  // position before that infers, the first one after does not.
  Game *played_game = config_game_create(config);
  game_seed(played_game, 31);
  draw_starting_racks(played_game);
  MoveList *move_list = move_list_create(1);
  Game *last_exchange_game = NULL;
  while (game_get_real_bag_size(played_game) >= RACK_SIZE) {
    game_destroy(last_exchange_game);
    last_exchange_game = game_duplicate(played_game);
    play_move(get_top_equity_move(played_game, move_list), played_game, NULL);
  }
  assert(!game_over(played_game));
  assert(last_exchange_game != NULL);
  pass_inference_test_assert_error(
      played_game, rvt, PASS_INFERENCE_DEFAULT_TEMPERATURE,
      ERROR_STATUS_PASS_INFERENCE_UNSUPPORTED_GAME);
  PassInference *last_exchange_inference = pass_inference_test_infer(
      last_exchange_game, rvt, PASS_INFERENCE_DEFAULT_TEMPERATURE,
      PASS_INFERENCE_DEFAULT_BONUS, 1);
  assert(pass_inference_get_number_of_racks(last_exchange_inference) > 0);
  pass_inference_destroy(last_exchange_inference);
  game_destroy(last_exchange_game);
  move_list_destroy(move_list);
  game_destroy(played_game);

  rack_value_table_destroy(rvt);
  config_destroy(config);
}

void test_pass_inference(void) {
  test_pass_inference_against_movegen();
  test_pass_inference_temperature_and_bonus();
  test_pass_inference_errors();
}
