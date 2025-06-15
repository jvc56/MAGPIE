#include <assert.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "../../src/def/inference_defs.h"
#include "../../src/def/letter_distribution_defs.h"

#include "../../src/ent/bag.h"
#include "../../src/ent/game.h"
#include "../../src/ent/inference_results.h"
#include "../../src/ent/klv.h"
#include "../../src/ent/letter_distribution.h"
#include "../../src/ent/player.h"
#include "../../src/ent/rack.h"
#include "../../src/ent/stats.h"

#include "../../src/impl/cgp.h"
#include "../../src/impl/config.h"

#include "../../src/util/io_util.h"
#include "../../src/util/math_util.h"
#include "../../src/util/string_util.h"

#include "test_constants.h"
#include "test_util.h"

error_code_t infer_for_test(const Config *config, int target_index,
                            int target_score, int target_num_exch,
                            const char *target_played_tiles_str,
                            InferenceResults *inference_results) {
  Game *game = config_get_game(config);
  const LetterDistribution *ld = game_get_ld(game);
  Rack *target_played_tiles = rack_create(ld_get_size(ld));
  if (target_played_tiles_str != NULL) {
    rack_set_to_string(ld, target_played_tiles, target_played_tiles_str);
  }
  ErrorStack *error_stack = error_stack_create();
  config_infer(config, target_index, target_score, target_num_exch,
               target_played_tiles, inference_results, error_stack);
  error_code_t status = error_stack_top(error_stack);
  rack_destroy(target_played_tiles);
  error_stack_destroy(error_stack);
  return status;
}

void test_trivial_random_probability(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  const LetterDistribution *ld = config_get_ld(config);
  int ld_size = ld_get_size(ld);
  Game *game = config_game_create(config);
  Rack *bag_as_rack = rack_create(ld_size);
  Rack *leave = rack_create(ld_size);

  // A minimum of zero should always be 100% probability
  assert(within_epsilon(get_probability_for_random_minimum_draw(
                            bag_as_rack, leave, ld_hl_to_ml(ld, "Z"), 0, 3),
                        1));
  assert(within_epsilon(get_probability_for_random_minimum_draw(
                            bag_as_rack, leave, ld_hl_to_ml(ld, "Z"), 0, 4),
                        1));
  assert(within_epsilon(get_probability_for_random_minimum_draw(
                            bag_as_rack, leave, ld_hl_to_ml(ld, "E"), 0, 6),
                        1));
  assert(within_epsilon(get_probability_for_random_minimum_draw(
                            bag_as_rack, leave, ld_hl_to_ml(ld, "E"), -1, 4),
                        1));

  // Minimum N where letters in bag is M and M > N
  // should always be 0
  assert(within_epsilon(get_probability_for_random_minimum_draw(
                            bag_as_rack, leave, ld_hl_to_ml(ld, "E"), 20, 4),
                        0));

  // If the player is emptying the bag and there are the minimum
  // number of leaves remaining, the probability is trivially 1.
  rack_add_letter(bag_as_rack, ld_hl_to_ml(ld, "E"));
  rack_add_letter(bag_as_rack, ld_hl_to_ml(ld, "E"));
  rack_add_letter(bag_as_rack, ld_hl_to_ml(ld, "E"));
  rack_add_letter(bag_as_rack, ld_hl_to_ml(ld, "E"));
  rack_add_letter(bag_as_rack, ld_hl_to_ml(ld, "E"));
  assert(within_epsilon(get_probability_for_random_minimum_draw(
                            bag_as_rack, leave, ld_hl_to_ml(ld, "E"), 4, 1),
                        1));

  rack_destroy(leave);
  rack_destroy(bag_as_rack);
  game_destroy(game);
  config_destroy(config);
}

void test_infer_rack_overflow(void) {
  Config *config =
      config_create_or_die("set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 "
                           "all -numplays 1 -threads 1");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  Game *game = config_get_game(config);
  const LetterDistribution *ld = config_get_ld(config);
  InferenceResults *inference_results = inference_results_create();

  error_code_t status =
      infer_for_test(config, 0, 0, 0, "ABCDEFGH", inference_results);
  assert(status == ERROR_STATUS_INFERENCE_RACK_OVERFLOW);
  game_reset(game);

  rack_set_to_string(ld, player_get_rack(game_get_player(game, 0)), "ABC");
  status = infer_for_test(config, 0, 0, 0, "DEFGH", inference_results);
  assert(status == ERROR_STATUS_INFERENCE_RACK_OVERFLOW);

  inference_results_destroy(inference_results);
  config_destroy(config);
}

void test_infer_no_tiles_played_rack_empty(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);

  InferenceResults *inference_results = inference_results_create();
  error_code_t status = infer_for_test(config, 0, 0, 0, "", inference_results);
  assert(status == ERROR_STATUS_INFERENCE_NO_TILES_PLAYED);

  inference_results_destroy(inference_results);
  config_destroy(config);
}

void test_infer_both_play_and_exchange(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);

  InferenceResults *inference_results = inference_results_create();
  error_code_t status =
      infer_for_test(config, 0, 0, 1, "DEFGH", inference_results);
  assert(status == ERROR_STATUS_INFERENCE_BOTH_PLAY_AND_EXCHANGE);

  inference_results_destroy(inference_results);
  config_destroy(config);
}

void test_infer_exchange_score_not_zero(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);

  InferenceResults *inference_results = inference_results_create();
  error_code_t status = infer_for_test(config, 0, 3, 1, "", inference_results);
  assert(status == ERROR_STATUS_INFERENCE_EXCHANGE_SCORE_NOT_ZERO);

  inference_results_destroy(inference_results);
  config_destroy(config);
}

void test_infer_exchange_not_board_is_letter_allowed_in_cross_set(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  Game *game = config_get_game(config);
  Bag *bag = game_get_bag(game);

  // There are 13 tiles in the bag
  load_cgp_or_die(game, VS_JEREMY);
  InferenceResults *inference_results = inference_results_create();
  error_code_t status = infer_for_test(config, 0, 3, 1, "", inference_results);
  assert(status == ERROR_STATUS_INFERENCE_EXCHANGE_NOT_ALLOWED);

  bag_add_letter(bag, BLANK_MACHINE_LETTER, 0);
  // There should now be 14 tiles in the bag
  status = infer_for_test(config, 0, 3, 1, "", inference_results);
  assert(status == ERROR_STATUS_INFERENCE_EXCHANGE_SCORE_NOT_ZERO);

  inference_results_destroy(inference_results);
  config_destroy(config);
}

void test_infer_tiles_played_not_in_bag(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);

  InferenceResults *inference_results = inference_results_create();
  load_and_exec_config_or_die(config, "set -eq 0 -threads 1");
  error_code_t status =
      infer_for_test(config, 0, 0, 1, "ACBYEYY", inference_results);
  assert(status == ERROR_STATUS_INFERENCE_TILES_PLAYED_NOT_IN_BAG);

  inference_results_destroy(inference_results);
  config_destroy(config);
}

void test_infer_nonerror_cases(int number_of_threads) {
  char *config_settings_str =
      get_formatted_string("set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 "
                           "all -numplays 1 -threads %d",
                           number_of_threads);
  Config *config = config_create_or_die(config_settings_str);
  free(config_settings_str);
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);

  Game *game = config_get_game(config);
  Bag *bag = game_get_bag(game);
  const LetterDistribution *ld = game_get_ld(game);
  int ld_size = ld_get_size(ld);
  Rack *rack = rack_create(ld_size);

  Player *player0 = game_get_player(game, 0);
  Player *player1 = game_get_player(game, 1);
  const KLV *klv = player_get_klv(player0);

  Rack *player0_rack = player_get_rack(player0);
  Rack *player1_rack = player_get_rack(player1);

  InferenceResults *inference_results = inference_results_create();
  Stat *letter_stat = stat_create(false);
  error_code_t status;
  LeaveRackList *lrl;

  load_and_exec_config_or_die(config, "set -numplays 20");
  status = infer_for_test(config, 0, 52, 0, "MUZAKS", inference_results);
  assert(status == ERROR_STATUS_SUCCESS);
  // With this rack, only keeping an S is possible, and
  // there are 3 S remaining.

  Stat *equity_values = inference_results_get_equity_values(
      inference_results, INFERENCE_TYPE_LEAVE);
  assert(stat_get_num_samples(equity_values) == 3);
  assert(stat_get_num_unique_samples(equity_values) == 1);
  rack_set_to_string(ld, rack, "S");
  assert(double_to_equity(stat_get_mean(equity_values)) ==
         klv_get_leave_value(klv, rack));
  for (int i = 0; i < ld_size; i++) {
    if (i == ld_hl_to_ml(ld, "S")) {
      assert(inference_results_get_subtotal(inference_results,
                                            INFERENCE_TYPE_LEAVE, i, 1,
                                            INFERENCE_SUBTOTAL_DRAW) == 3);
      assert(inference_results_get_subtotal(inference_results,
                                            INFERENCE_TYPE_LEAVE, i, 1,
                                            INFERENCE_SUBTOTAL_LEAVE) == 1);
    } else {
      assert(inference_results_get_subtotal(inference_results,
                                            INFERENCE_TYPE_LEAVE, i, 1,
                                            INFERENCE_SUBTOTAL_DRAW) == 0);
    }
  }
  inference_results_set_stat_for_letter(inference_results, INFERENCE_TYPE_LEAVE,
                                        letter_stat, ld_hl_to_ml(ld, "S"));
  assert(within_epsilon(stat_get_mean(letter_stat), 1));
  assert(within_epsilon(stat_get_stdev(letter_stat), 0));
  assert(within_epsilon(
      get_probability_for_random_minimum_draw(
          inference_results_get_bag_as_rack(inference_results),
          inference_results_get_target_known_unplayed_tiles(inference_results),
          ld_hl_to_ml(ld, "S"), 1, 6),
      (double)3 / 94));

  lrl = inference_results_get_leave_rack_list(inference_results);

  assert(leave_rack_list_get_count(lrl) == 1);
  assert(rack_get_total_letters(
             leave_rack_get_leave(leave_rack_list_get_rack(lrl, 0))) == 1);
  assert(rack_get_letter(leave_rack_get_leave(leave_rack_list_get_rack(lrl, 0)),
                         ld_hl_to_ml(ld, "S")) == 1);

  // Both game racks should be empty
  assert(rack_get_total_letters(player0_rack) == 0);
  assert(rack_get_total_letters(player1_rack) == 0);

  status = infer_for_test(config, 0, 58, 0, "MUZAKY", inference_results);
  assert(status == ERROR_STATUS_SUCCESS);
  // Letters not possible:
  // A - YAKUZA
  // B - ZAMBUK
  // K - none in bag
  // Q - QUAKY
  // Z - none in bag
  // Refetch equity values because the underlying
  // inference_results results were recreated
  equity_values = inference_results_get_equity_values(inference_results,
                                                      INFERENCE_TYPE_LEAVE);
  assert(stat_get_num_samples(equity_values) == 83);
  assert(stat_get_num_unique_samples(equity_values) == 22);
  for (int i = 0; i < ld_size; i++) {
    if (i == ld_hl_to_ml(ld, "A") || i == ld_hl_to_ml(ld, "B") ||
        i == ld_hl_to_ml(ld, "K") || i == ld_hl_to_ml(ld, "Q") ||
        i == ld_hl_to_ml(ld, "Z")) {
      assert(inference_results_get_subtotal(inference_results,
                                            INFERENCE_TYPE_LEAVE, i, 1,
                                            INFERENCE_SUBTOTAL_DRAW) == 0);
      assert(inference_results_get_subtotal(inference_results,
                                            INFERENCE_TYPE_LEAVE, i, 1,
                                            INFERENCE_SUBTOTAL_LEAVE) == 0);
    } else {
      assert(inference_results_get_subtotal(inference_results,
                                            INFERENCE_TYPE_LEAVE, i, 1,
                                            INFERENCE_SUBTOTAL_DRAW) != 0);
    }
  }
  inference_results_set_stat_for_letter(inference_results, INFERENCE_TYPE_LEAVE,
                                        letter_stat, ld_hl_to_ml(ld, "E"));
  assert(within_epsilon(stat_get_mean(letter_stat), (double)12 / 83));
  assert(within_epsilon(
      get_probability_for_random_minimum_draw(
          inference_results_get_bag_as_rack(inference_results),
          inference_results_get_target_known_unplayed_tiles(inference_results),
          ld_hl_to_ml(ld, "Q"), 1, 6),
      (double)1 / 94));
  assert(within_epsilon(
      get_probability_for_random_minimum_draw(
          inference_results_get_bag_as_rack(inference_results),
          inference_results_get_target_known_unplayed_tiles(inference_results),
          ld_hl_to_ml(ld, "B"), 1, 6),
      (double)2 / 94));

  lrl = inference_results_get_leave_rack_list(inference_results);

  assert(leave_rack_list_get_count(lrl) == 20);
  assert(rack_get_total_letters(
             leave_rack_get_leave(leave_rack_list_get_rack(lrl, 0))) == 1);
  assert(rack_get_letter(leave_rack_get_leave(leave_rack_list_get_rack(lrl, 0)),
                         ld_hl_to_ml(ld, "E")) == 1);
  assert(rack_get_letter(leave_rack_get_leave(leave_rack_list_get_rack(lrl, 1)),
                         ld_hl_to_ml(ld, "I")) == 1);

  // Both game racks should be empty
  assert(rack_get_total_letters(player0_rack) == 0);
  assert(rack_get_total_letters(player1_rack) == 0);

  status = infer_for_test(config, 0, 50, 0, "MUZAK", inference_results);
  assert(status == ERROR_STATUS_SUCCESS);
  // Can't have B or Y because of ZAMBUK and MUZAKY
  // Can't have K or Z because there are none in the bag
  for (int i = 0; i < ld_size; i++) {
    if (i == ld_hl_to_ml(ld, "B") || i == ld_hl_to_ml(ld, "K") ||
        i == ld_hl_to_ml(ld, "Y") || i == ld_hl_to_ml(ld, "Z")) {
      assert(inference_results_get_subtotal(inference_results,
                                            INFERENCE_TYPE_LEAVE, i, 1,
                                            INFERENCE_SUBTOTAL_DRAW) == 0);
    } else {
      assert(inference_results_get_subtotal(inference_results,
                                            INFERENCE_TYPE_LEAVE, i, 1,
                                            INFERENCE_SUBTOTAL_DRAW) != 0);
    }
  }
  assert(within_epsilon(
      get_probability_for_random_minimum_draw(
          inference_results_get_bag_as_rack(inference_results),
          inference_results_get_target_known_unplayed_tiles(inference_results),
          ld_hl_to_ml(ld, "B"), 2, 5),
      (double)1 / choose(95, 2)));
  // Both game racks should be empty
  assert(rack_get_total_letters(player0_rack) == 0);
  assert(rack_get_total_letters(player1_rack) == 0);

  load_cgp_or_die(game, VS_JEREMY_WITH_P2_RACK);
  // Score doesn't matter since the bag
  // is empty and the inference_results should just be
  // the remaining tiles exactly. Since the played
  // tiles contain an E, the inferred leave should not
  // contain an E.
  load_and_exec_config_or_die(config, "set -eq 0");
  status = infer_for_test(config, 0, 22, 0, "E", inference_results);
  assert(status == ERROR_STATUS_SUCCESS);
  // Refetch equity values because the underlying
  // inference_results results were recreated
  equity_values = inference_results_get_equity_values(inference_results,
                                                      INFERENCE_TYPE_LEAVE);
  assert(stat_get_num_samples(equity_values) == 1);
  assert(stat_get_num_unique_samples(equity_values) == 1);
  rack_set_to_string(ld, rack, "DDSW??");
  assert(double_to_equity(stat_get_mean(equity_values)) ==
         klv_get_leave_value(klv, rack));
  for (int i = 0; i < ld_size; i++) {
    if (i == ld_hl_to_ml(ld, "S") || i == ld_hl_to_ml(ld, "W")) {
      assert(inference_results_get_subtotal(inference_results,
                                            INFERENCE_TYPE_LEAVE, i, 1,
                                            INFERENCE_SUBTOTAL_DRAW) == 1);
      assert(inference_results_get_subtotal(inference_results,
                                            INFERENCE_TYPE_LEAVE, i, 1,
                                            INFERENCE_SUBTOTAL_LEAVE) == 1);
    } else if (i == ld_hl_to_ml(ld, "D") || i == ld_hl_to_ml(ld, "?")) {
      assert(inference_results_get_subtotal(inference_results,
                                            INFERENCE_TYPE_LEAVE, i, 2,
                                            INFERENCE_SUBTOTAL_DRAW) == 1);
      assert(inference_results_get_subtotal(inference_results,
                                            INFERENCE_TYPE_LEAVE, i, 2,
                                            INFERENCE_SUBTOTAL_LEAVE) == 1);
    } else {
      assert(inference_results_get_subtotal(inference_results,
                                            INFERENCE_TYPE_LEAVE, i, 1,
                                            INFERENCE_SUBTOTAL_DRAW) == 0);
    }
  }
  game_reset(game);

  load_and_exec_config_or_die(config, "set -numplays 100");
  status = infer_for_test(config, 0, 8, 0, "ENRT", inference_results);
  assert(status == ERROR_STATUS_SUCCESS);
  // There are only 3 racks for which playing RENT for 8 on the opening is
  // top equity:
  // 1) ?ENNRRT keeping ?NR = 2 * 5 * 5  = 50 possible draws
  // 2) EENRRTT keeping ERT = 11 * 5 * 5 = 275 possible draws
  // 3) ENNRRTT keeping NRT = 5 * 5 * 5  = 125 possible draws which sums to 450
  // total draws. We use this case to easily check that the combinatorial math
  // is correct.
  // Refetch equity values because the underlying inference_results
  // results were recreated
  equity_values = inference_results_get_equity_values(inference_results,
                                                      INFERENCE_TYPE_LEAVE);
  assert(stat_get_num_samples(equity_values) == 450);
  assert(stat_get_num_unique_samples(equity_values) == 3);
  for (int i = 0; i < ld_size; i++) {
    if (i == ld_hl_to_ml(ld, "?")) {
      // The blank was only in leave 1
      assert(inference_results_get_subtotal(inference_results,
                                            INFERENCE_TYPE_LEAVE, i, 1,
                                            INFERENCE_SUBTOTAL_DRAW) == 50);
      assert(inference_results_get_subtotal(inference_results,
                                            INFERENCE_TYPE_LEAVE, i, 1,
                                            INFERENCE_SUBTOTAL_LEAVE) == 1);
    } else if (i == ld_hl_to_ml(ld, "E")) {
      // The E was only in leave 2
      assert(inference_results_get_subtotal(inference_results,
                                            INFERENCE_TYPE_LEAVE, i, 1,
                                            INFERENCE_SUBTOTAL_DRAW) == 275);
      assert(inference_results_get_subtotal(inference_results,
                                            INFERENCE_TYPE_LEAVE, i, 1,
                                            INFERENCE_SUBTOTAL_LEAVE) == 1);
    } else if (i == ld_hl_to_ml(ld, "N")) {
      // The N was in leaves 1 and 3
      assert(inference_results_get_subtotal(inference_results,
                                            INFERENCE_TYPE_LEAVE, i, 1,
                                            INFERENCE_SUBTOTAL_DRAW) == 175);
      assert(inference_results_get_subtotal(inference_results,
                                            INFERENCE_TYPE_LEAVE, i, 1,
                                            INFERENCE_SUBTOTAL_LEAVE) == 2);
    } else if (i == ld_hl_to_ml(ld, "R")) {
      // The R was found in all of the leaves
      assert(inference_results_get_subtotal(inference_results,
                                            INFERENCE_TYPE_LEAVE, i, 1,
                                            INFERENCE_SUBTOTAL_DRAW) == 450);
      assert(inference_results_get_subtotal(inference_results,
                                            INFERENCE_TYPE_LEAVE, i, 1,
                                            INFERENCE_SUBTOTAL_LEAVE) == 3);
    } else if (i == ld_hl_to_ml(ld, "T")) {
      assert(inference_results_get_subtotal(inference_results,
                                            INFERENCE_TYPE_LEAVE, i, 1,
                                            INFERENCE_SUBTOTAL_DRAW) == 400);
    } else {
      assert(inference_results_get_subtotal(inference_results,
                                            INFERENCE_TYPE_LEAVE, i, 1,
                                            INFERENCE_SUBTOTAL_DRAW) == 0);
      assert(inference_results_get_subtotal(inference_results,
                                            INFERENCE_TYPE_LEAVE, i, 1,
                                            INFERENCE_SUBTOTAL_LEAVE) == 0);
    }
  }
  inference_results_set_stat_for_letter(inference_results, INFERENCE_TYPE_LEAVE,
                                        letter_stat, ld_hl_to_ml(ld, "E"));
  assert(within_epsilon(stat_get_mean(letter_stat), (double)275 / 450));
  assert(within_epsilon(stat_get_stdev(letter_stat), 0.48749802152178456360));
  inference_results_set_stat_for_letter(inference_results, INFERENCE_TYPE_LEAVE,
                                        letter_stat, ld_hl_to_ml(ld, "R"));
  assert(within_epsilon(stat_get_mean(letter_stat), 1));
  assert(within_epsilon(stat_get_stdev(letter_stat), 0));

  assert_equal_at_equity_resolution(stat_get_stdev(equity_values),
                                    6.53225818584641171327);

  lrl = inference_results_get_leave_rack_list(inference_results);

  assert(leave_rack_list_get_count(lrl) == 3);

  assert(rack_get_total_letters(
             leave_rack_get_leave(leave_rack_list_get_rack(lrl, 0))) == 3);
  assert(rack_get_letter(leave_rack_get_leave(leave_rack_list_get_rack(lrl, 0)),
                         ld_hl_to_ml(ld, "E")) == 1);
  assert(rack_get_letter(leave_rack_get_leave(leave_rack_list_get_rack(lrl, 0)),
                         ld_hl_to_ml(ld, "R")) == 1);
  assert(rack_get_letter(leave_rack_get_leave(leave_rack_list_get_rack(lrl, 0)),
                         ld_hl_to_ml(ld, "T")) == 1);

  assert(rack_get_total_letters(
             leave_rack_get_leave(leave_rack_list_get_rack(lrl, 1))) == 3);
  assert(rack_get_letter(leave_rack_get_leave(leave_rack_list_get_rack(lrl, 1)),
                         ld_hl_to_ml(ld, "N")) == 1);
  assert(rack_get_letter(leave_rack_get_leave(leave_rack_list_get_rack(lrl, 1)),
                         ld_hl_to_ml(ld, "R")) == 1);
  assert(rack_get_letter(leave_rack_get_leave(leave_rack_list_get_rack(lrl, 1)),
                         ld_hl_to_ml(ld, "T")) == 1);

  assert(rack_get_total_letters(
             leave_rack_get_leave(leave_rack_list_get_rack(lrl, 2))) == 3);
  assert(rack_get_letter(leave_rack_get_leave(leave_rack_list_get_rack(lrl, 2)),
                         ld_hl_to_ml(ld, "N")) == 1);
  assert(rack_get_letter(leave_rack_get_leave(leave_rack_list_get_rack(lrl, 2)),
                         ld_hl_to_ml(ld, "R")) == 1);
  assert(rack_get_letter(leave_rack_get_leave(leave_rack_list_get_rack(lrl, 2)),
                         BLANK_MACHINE_LETTER) == 1);

  // Contrive an impossible situation to easily test
  // more combinatorics
  load_cgp_or_die(game, OOPSYCHOLOGY_CGP);

  // Empty the bag
  while (!bag_is_empty(bag)) {
    bag_draw_random_letter(bag, 0);
  }

  bag_add_letter(bag, ld_hl_to_ml(ld, "I"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "I"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "I"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "I"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "I"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "I"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "I"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "E"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "E"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "E"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "E"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "Z"), 0);

  // Z(OOPSYCHOLOGY) is over 100 points so keeping the Z will never be
  // inferred
  // for plays scoring 50.
  load_and_exec_config_or_die(config, "set -eq 0");
  status = infer_for_test(config, 0, 50, 0, "IIII", inference_results);
  assert(status == ERROR_STATUS_SUCCESS);
  // There are only 4 racks for which not playing Z(OOPSYCHOLOGY) is
  // correct:
  // EEEIIII = 4 possible draws for E = 4 total
  // EEIIIII = 6 possible draws for the Es * 3 possible draws for the Is =
  // 18
  // total EIIIIII = 4 possible draws for E = 4  * 3 possible draws for the
  // Is =
  // 12 IIIIIII = 1 possible draw for the Is = 1 For a total of 35
  // possible draws
  // Refetch equity values because the underlying
  // inference_results results were recreated
  equity_values = inference_results_get_equity_values(inference_results,
                                                      INFERENCE_TYPE_LEAVE);
  assert(stat_get_num_samples(equity_values) == 35);
  assert(stat_get_num_unique_samples(equity_values) == 4);
  for (int i = 0; i < ld_size; i++) {
    if (i == ld_hl_to_ml(ld, "E")) {
      assert(inference_results_get_subtotal(inference_results,
                                            INFERENCE_TYPE_LEAVE, i, 1,
                                            INFERENCE_SUBTOTAL_DRAW) == 12);
      assert(inference_results_get_subtotal_sum_with_minimum(
                 inference_results, INFERENCE_TYPE_LEAVE, i, 1,
                 INFERENCE_SUBTOTAL_DRAW) == 34);
      assert(inference_results_get_subtotal(inference_results,
                                            INFERENCE_TYPE_LEAVE, i, 1,
                                            INFERENCE_SUBTOTAL_LEAVE) == 1);
      assert(inference_results_get_subtotal_sum_with_minimum(
                 inference_results, INFERENCE_TYPE_LEAVE, i, 1,
                 INFERENCE_SUBTOTAL_LEAVE) == 3);

      assert(inference_results_get_subtotal(inference_results,
                                            INFERENCE_TYPE_LEAVE, i, 2,
                                            INFERENCE_SUBTOTAL_DRAW) == 18);
      assert(inference_results_get_subtotal_sum_with_minimum(
                 inference_results, INFERENCE_TYPE_LEAVE, i, 2,
                 INFERENCE_SUBTOTAL_DRAW) == 22);
      assert(inference_results_get_subtotal(inference_results,
                                            INFERENCE_TYPE_LEAVE, i, 2,
                                            INFERENCE_SUBTOTAL_LEAVE) == 1);
      assert(inference_results_get_subtotal_sum_with_minimum(
                 inference_results, INFERENCE_TYPE_LEAVE, i, 2,
                 INFERENCE_SUBTOTAL_LEAVE) == 2);

      assert(inference_results_get_subtotal(inference_results,
                                            INFERENCE_TYPE_LEAVE, i, 3,
                                            INFERENCE_SUBTOTAL_DRAW) == 4);
      assert(inference_results_get_subtotal_sum_with_minimum(
                 inference_results, INFERENCE_TYPE_LEAVE, i, 3,
                 INFERENCE_SUBTOTAL_DRAW) == 4);
      assert(inference_results_get_subtotal(inference_results,
                                            INFERENCE_TYPE_LEAVE, i, 3,
                                            INFERENCE_SUBTOTAL_LEAVE) == 1);
      assert(inference_results_get_subtotal_sum_with_minimum(
                 inference_results, INFERENCE_TYPE_LEAVE, i, 3,
                 INFERENCE_SUBTOTAL_LEAVE) == 1);
    } else if (i == ld_hl_to_ml(ld, "I")) {
      assert(inference_results_get_subtotal(inference_results,
                                            INFERENCE_TYPE_LEAVE, i, 1,
                                            INFERENCE_SUBTOTAL_DRAW) == 18);
      assert(inference_results_get_subtotal_sum_with_minimum(
                 inference_results, INFERENCE_TYPE_LEAVE, i, 1,
                 INFERENCE_SUBTOTAL_DRAW) == 31);
      assert(inference_results_get_subtotal(inference_results,
                                            INFERENCE_TYPE_LEAVE, i, 1,
                                            INFERENCE_SUBTOTAL_LEAVE) == 1);
      assert(inference_results_get_subtotal_sum_with_minimum(
                 inference_results, INFERENCE_TYPE_LEAVE, i, 1,
                 INFERENCE_SUBTOTAL_LEAVE) == 3);

      assert(inference_results_get_subtotal(inference_results,
                                            INFERENCE_TYPE_LEAVE, i, 2,
                                            INFERENCE_SUBTOTAL_DRAW) == 12);
      assert(inference_results_get_subtotal_sum_with_minimum(
                 inference_results, INFERENCE_TYPE_LEAVE, i, 2,
                 INFERENCE_SUBTOTAL_DRAW) == 13);
      assert(inference_results_get_subtotal(inference_results,
                                            INFERENCE_TYPE_LEAVE, i, 2,
                                            INFERENCE_SUBTOTAL_LEAVE) == 1);
      assert(inference_results_get_subtotal_sum_with_minimum(
                 inference_results, INFERENCE_TYPE_LEAVE, i, 2,
                 INFERENCE_SUBTOTAL_LEAVE) == 2);

      assert(inference_results_get_subtotal(inference_results,
                                            INFERENCE_TYPE_LEAVE, i, 3,
                                            INFERENCE_SUBTOTAL_DRAW) == 1);
      assert(inference_results_get_subtotal_sum_with_minimum(
                 inference_results, INFERENCE_TYPE_LEAVE, i, 3,
                 INFERENCE_SUBTOTAL_DRAW) == 1);
      assert(inference_results_get_subtotal(inference_results,
                                            INFERENCE_TYPE_LEAVE, i, 3,
                                            INFERENCE_SUBTOTAL_LEAVE) == 1);
      assert(inference_results_get_subtotal_sum_with_minimum(
                 inference_results, INFERENCE_TYPE_LEAVE, i, 3,
                 INFERENCE_SUBTOTAL_LEAVE) == 1);
    } else {
      assert(inference_results_get_subtotal(inference_results,
                                            INFERENCE_TYPE_LEAVE, i, 1,
                                            INFERENCE_SUBTOTAL_DRAW) == 0);
      assert(inference_results_get_subtotal_sum_with_minimum(
                 inference_results, INFERENCE_TYPE_LEAVE, i, 1,
                 INFERENCE_SUBTOTAL_DRAW) == 0);
      assert(inference_results_get_subtotal(inference_results,
                                            INFERENCE_TYPE_LEAVE, i, 1,
                                            INFERENCE_SUBTOTAL_LEAVE) == 0);
      assert(inference_results_get_subtotal_sum_with_minimum(
                 inference_results, INFERENCE_TYPE_LEAVE, i, 1,
                 INFERENCE_SUBTOTAL_LEAVE) == 0);
    }
  }
  assert(within_epsilon(
      get_probability_for_random_minimum_draw(
          inference_results_get_bag_as_rack(inference_results),
          inference_results_get_target_known_unplayed_tiles(inference_results),
          ld_hl_to_ml(ld, "E"), 3, 4),
      (double)4 / choose(8, 3)));
  game_reset(game);

  // Check that the equity margin works
  load_and_exec_config_or_die(config, "set -eq 5");
  status = infer_for_test(config, 0, 58, 0, "MUZAKY", inference_results);
  assert(status == ERROR_STATUS_SUCCESS);
  // Letters not possible with equity margin of 5:
  // B - ZAMBUK
  // K - none in bag
  // Q - QUAKY
  // Z - none in bag
  // Letters now possible because of the additional 5 equity buffer:
  // A - YAKUZA
  // 2 Bs and 1 Q with 6 played tiles is 100 - (2 + 1 + 6) = 91
  // Refetch equity values because the underlying
  // inference_results results were recreated
  equity_values = inference_results_get_equity_values(inference_results,
                                                      INFERENCE_TYPE_LEAVE);
  assert(stat_get_num_samples(equity_values) == 91);
  // All letters except the 4 described above are possible, so 27 - 4 = 23
  assert(stat_get_num_unique_samples(equity_values) == 23);
  for (int i = 0; i < ld_size; i++) {
    if (i == ld_hl_to_ml(ld, "B") || i == ld_hl_to_ml(ld, "K") ||
        i == ld_hl_to_ml(ld, "Q") || i == ld_hl_to_ml(ld, "Z")) {
      assert(inference_results_get_subtotal(inference_results,
                                            INFERENCE_TYPE_LEAVE, i, 1,
                                            INFERENCE_SUBTOTAL_DRAW) == 0);
    } else {
      assert(inference_results_get_subtotal(inference_results,
                                            INFERENCE_TYPE_LEAVE, i, 1,
                                            INFERENCE_SUBTOTAL_DRAW) != 0);
    }
  }

  // Test partial leaves
  // play GRIND with partial leave of ?
  // Partially known leaves that are on the player's rack
  // before the inference_results are not removed from the bag, so
  // we have to remove it here.
  rack_set_to_string(ld, player0_rack, "?");
  bag_draw_letter(bag, ld_hl_to_ml(ld, "?"), 0);
  load_and_exec_config_or_die(config, "set -eq 0");
  status = infer_for_test(config, 0, 18, 0, "GRIND", inference_results);
  assert(status == ERROR_STATUS_SUCCESS);
  // If GRIND is played keeping ?, the only
  // possible other tile is an X
  // Refetch equity values because the underlying
  // inference_results results were recreated
  equity_values = inference_results_get_equity_values(inference_results,
                                                      INFERENCE_TYPE_LEAVE);
  assert(stat_get_num_samples(equity_values) == 2);
  assert(stat_get_num_unique_samples(equity_values) == 1);
  rack_set_to_string(ld, rack, "X?");
  assert(double_to_equity(stat_get_mean(equity_values)) ==
         klv_get_leave_value(klv, rack));
  for (int i = 0; i < ld_size; i++) {
    if (i == ld_hl_to_ml(ld, "?") || i == ld_hl_to_ml(ld, "X")) {
      assert(inference_results_get_subtotal(inference_results,
                                            INFERENCE_TYPE_LEAVE, i, 1,
                                            INFERENCE_SUBTOTAL_DRAW) == 2);
      assert(inference_results_get_subtotal(inference_results,
                                            INFERENCE_TYPE_LEAVE, i, 1,
                                            INFERENCE_SUBTOTAL_LEAVE) == 1);
    } else {
      assert(inference_results_get_subtotal(inference_results,
                                            INFERENCE_TYPE_LEAVE, i, 1,
                                            INFERENCE_SUBTOTAL_DRAW) == 0);
    }
  }
  game_reset(game);

  rack_set_to_string(ld, player0_rack, "H");
  bag_draw_letter(bag, ld_hl_to_ml(ld, "H"), 0);
  load_and_exec_config_or_die(config, "set -eq 0");
  status = infer_for_test(config, 0, 6, 0, "RIN", inference_results);
  // If the player opens with RIN for 6 keeping an H, there are only 3
  // possible racks where this would be correct:
  // 1) ?HIINRR keeping ?HIR = 2 * 2 * 8 * 5 = 160
  // 2) ?HINNRR keeping ?HNR = 2 * 2 * 5 * 5 = 100
  // 3) HIINNRR keeping HINR = 2 * 8 * 5 * 5 = 400
  // For a total of 660 possible draws
  assert(status == ERROR_STATUS_SUCCESS);
  // Refetch equity values because the underlying
  // inference_results results were recreated
  equity_values = inference_results_get_equity_values(inference_results,
                                                      INFERENCE_TYPE_LEAVE);
  assert(stat_get_num_samples(equity_values) == 660);
  assert(stat_get_num_unique_samples(equity_values) == 3);
  rack_set_to_string(ld, rack, "?HIR");
  const double bhir_value = equity_to_double(klv_get_leave_value(klv, rack));
  const double bhir_prop_value = bhir_value * 160;
  rack_set_to_string(ld, rack, "?HNR");
  const double bhnr_value = equity_to_double(klv_get_leave_value(klv, rack));
  const double bhnr_prop_value = bhnr_value * 100;
  rack_set_to_string(ld, rack, "HINR");
  const double hirn_value = equity_to_double(klv_get_leave_value(klv, rack));
  const double hirn_prop_value = hirn_value * 400;
  const double mean_rin_leave_value =
      (bhir_prop_value + bhnr_prop_value + hirn_prop_value) / 660;
  assert_equal_at_equity_resolution(stat_get_mean(equity_values),
                                    mean_rin_leave_value);

  // Test exchanges
  load_cgp_or_die(game, VS_JEREMY);
  // Take out good letters and throw in bad ones to force certain
  // racks to have exchange as the best play
  bag_draw_letter(bag, ld_hl_to_ml(ld, "?"), 0);
  bag_draw_letter(bag, ld_hl_to_ml(ld, "?"), 0);
  bag_draw_letter(bag, ld_hl_to_ml(ld, "E"), 0);
  bag_draw_letter(bag, ld_hl_to_ml(ld, "A"), 0);

  bag_add_letter(bag, ld_hl_to_ml(ld, "Q"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "W"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "W"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "V"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "V"), 0);

  rack_reset(rack);
  load_and_exec_config_or_die(config, "set -eq 0");
  status = infer_for_test(config, 0, 0, 6, "", inference_results);

  assert(status == ERROR_STATUS_SUCCESS);
  // Keeping any one of D, H, R, or S is valid
  assert(inference_results_get_subtotal(inference_results, INFERENCE_TYPE_LEAVE,
                                        ld_hl_to_ml(ld, "D"), 1,
                                        INFERENCE_SUBTOTAL_DRAW) != 0);
  assert(inference_results_get_subtotal(inference_results, INFERENCE_TYPE_LEAVE,
                                        ld_hl_to_ml(ld, "H"), 1,
                                        INFERENCE_SUBTOTAL_DRAW) != 0);
  assert(inference_results_get_subtotal(inference_results, INFERENCE_TYPE_LEAVE,
                                        ld_hl_to_ml(ld, "R"), 1,
                                        INFERENCE_SUBTOTAL_DRAW) != 0);
  assert(inference_results_get_subtotal(inference_results, INFERENCE_TYPE_LEAVE,
                                        ld_hl_to_ml(ld, "S"), 1,
                                        INFERENCE_SUBTOTAL_DRAW) != 0);

  // There are exchanges where throwing back at least one
  // of these is correct
  assert(inference_results_get_subtotal(
             inference_results, INFERENCE_TYPE_EXCHANGED, ld_hl_to_ml(ld, "D"),
             1, INFERENCE_SUBTOTAL_DRAW) != 0);
  assert(inference_results_get_subtotal(
             inference_results, INFERENCE_TYPE_EXCHANGED, ld_hl_to_ml(ld, "L"),
             1, INFERENCE_SUBTOTAL_DRAW) != 0);
  assert(inference_results_get_subtotal(
             inference_results, INFERENCE_TYPE_EXCHANGED, ld_hl_to_ml(ld, "Q"),
             1, INFERENCE_SUBTOTAL_DRAW) != 0);
  assert(inference_results_get_subtotal(
             inference_results, INFERENCE_TYPE_EXCHANGED, ld_hl_to_ml(ld, "V"),
             1, INFERENCE_SUBTOTAL_DRAW) != 0);
  assert(inference_results_get_subtotal(
             inference_results, INFERENCE_TYPE_EXCHANGED, ld_hl_to_ml(ld, "W"),
             1, INFERENCE_SUBTOTAL_DRAW) != 0);

  // Exchanges with the I are never correct
  assert(inference_results_get_subtotal(inference_results, INFERENCE_TYPE_LEAVE,
                                        ld_hl_to_ml(ld, "I"), 1,
                                        INFERENCE_SUBTOTAL_DRAW) == 0);
  assert(inference_results_get_subtotal(
             inference_results, INFERENCE_TYPE_EXCHANGED, ld_hl_to_ml(ld, "I"),
             1, INFERENCE_SUBTOTAL_DRAW) == 0);

  game_reset(game);

  stat_destroy(letter_stat);
  rack_destroy(rack);
  inference_results_destroy(inference_results);
  config_destroy(config);
}

void test_infer(void) {
  test_trivial_random_probability();
  test_infer_rack_overflow();
  test_infer_no_tiles_played_rack_empty();
  test_infer_both_play_and_exchange();
  test_infer_exchange_score_not_zero();
  test_infer_exchange_not_board_is_letter_allowed_in_cross_set();
  test_infer_tiles_played_not_in_bag();
  test_infer_nonerror_cases(1);
  test_infer_nonerror_cases(2);
  test_infer_nonerror_cases(7);
}
