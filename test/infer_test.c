#include "../src/compat/ctime.h"
#include "../src/def/inference_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/alias_method.h"
#include "../src/ent/bag.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/game_history.h"
#include "../src/ent/inference_results.h"
#include "../src/ent/klv.h"
#include "../src/ent/leave_rack.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/stats.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/xoshiro.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/inference.h"
#include "../src/impl/move_gen.h"
#include "../src/str/rack_string.h"
#include "../src/util/io_util.h"
#include "../src/util/math_util.h"
#include "../src/util/string_util.h"
#include "test_constants.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

int leave_rack_get_leave_total_letters(const LeaveRack *leave_rack,
                                       const LetterDistribution *ld) {
  Rack rack;
  rack_set_dist_size(&rack, ld_get_size(ld));
  leave_rack_get_leave(leave_rack, &rack);
  return rack_get_total_letters(&rack);
}

int leave_rack_get_leave_letter(const LeaveRack *leave_rack, MachineLetter ml,
                                const LetterDistribution *ld) {
  Rack rack;
  rack_set_dist_size(&rack, ld_get_size(ld));
  leave_rack_get_leave(leave_rack, &rack);
  return rack_get_letter(&rack, ml);
}




void test_leave_rack_reset(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  const LetterDistribution *ld = config_get_ld(config);
  int ld_size = ld_get_size(ld);

  Rack rack;
  rack_set_dist_size_and_reset(&rack, ld_size);

  LeaveRackList *lrl = leave_rack_list_create(10);

  const int capacities[] = {10, 1, 3, 5, 20, 2, 15, 14, 9, 100, 50, 3, -1};
  int i = 0;
  while (true) {
    const int capacity = capacities[i];
    if (capacity == -1) {
      break;
    }
    leave_rack_list_reset(lrl, capacity);
    for (int j = 0; j < capacity; j++) {
      // The contents of the rack and the count doesn't matter since we are just
      // testing that the list resizing works.
      leave_rack_list_insert_rack(&rack, &rack, 0, 0, lrl);
    }
    i++;
  }

  leave_rack_list_destroy(lrl);
  config_destroy(config);
}

void test_trivial_random_probability(void) {
  Config *config =
      config_create_or_die("set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 "
                           "all -r2 all -numplays 1");
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
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 "
      "all -numplays 1 -threads 1");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  Game *game = config_get_game(config);
  InferenceResults *inference_results = inference_results_create(NULL);

  error_code_t status =
      infer_for_test(config, 0, 0, 0, "ABCDEFGH", "", "", inference_results);
  assert(status == ERROR_STATUS_INFERENCE_RACK_OVERFLOW);
  game_reset(game);

  status =
      infer_for_test(config, 0, 0, 0, "DEFGH", "ABC", "", inference_results);
  assert(status == ERROR_STATUS_INFERENCE_RACK_OVERFLOW);

  inference_results_destroy(inference_results);
  config_destroy(config);
}

void test_infer_no_tiles_played_rack_empty(void) {
  Config *config =
      config_create_or_die("set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 "
                           "all -r2 all -numplays 1");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);

  ErrorStack *error_stack = error_stack_create();
  InferenceResults *inference_results = inference_results_create(NULL);
  error_code_t status =
      infer_for_test(config, 0, 0, 0, "", "", "", inference_results);
  assert(status == ERROR_STATUS_INFERENCE_NO_TILES_PLAYED);

  error_stack_destroy(error_stack);
  inference_results_destroy(inference_results);
  config_destroy(config);
}

void test_infer_both_play_and_exchange(void) {
  Config *config =
      config_create_or_die("set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 "
                           "all -r2 all -numplays 1");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);

  InferenceResults *inference_results = inference_results_create(NULL);
  error_code_t status =
      infer_for_test(config, 0, 0, 1, "DEFGH", "", "", inference_results);
  assert(status == ERROR_STATUS_INFERENCE_BOTH_PLAY_AND_EXCHANGE);

  inference_results_destroy(inference_results);
  config_destroy(config);
}

void test_infer_exchange_score_not_zero(void) {
  Config *config =
      config_create_or_die("set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 "
                           "all -r2 all -numplays 1");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);

  InferenceResults *inference_results = inference_results_create(NULL);
  error_code_t status =
      infer_for_test(config, 0, 3, 1, "", "", "", inference_results);
  assert(status == ERROR_STATUS_INFERENCE_EXCHANGE_SCORE_NOT_ZERO);

  inference_results_destroy(inference_results);
  config_destroy(config);
}

void test_infer_exchange_not_board_is_letter_allowed_in_cross_set(void) {
  Config *config =
      config_create_or_die("set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 "
                           "all -r2 all -numplays 1");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  Game *game = config_get_game(config);
  Bag *bag = game_get_bag(game);

  // There are 13 tiles in the bag
  load_cgp_or_die(game, VS_JEREMY);
  InferenceResults *inference_results = inference_results_create(NULL);
  error_code_t status =
      infer_for_test(config, 0, 3, 1, "", "", "", inference_results);
  assert(status == ERROR_STATUS_INFERENCE_EXCHANGE_NOT_ALLOWED);

  bag_add_letter(bag, BLANK_MACHINE_LETTER, 0);
  // There should now be 14 tiles in the bag
  status = infer_for_test(config, 0, 3, 1, "", "", "", inference_results);
  assert(status == ERROR_STATUS_INFERENCE_EXCHANGE_SCORE_NOT_ZERO);

  inference_results_destroy(inference_results);
  config_destroy(config);
}

void test_infer_tiles_played_not_in_bag(void) {
  Config *config =
      config_create_or_die("set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 "
                           "all -r2 all -numplays 1");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);

  InferenceResults *inference_results = inference_results_create(NULL);
  load_and_exec_config_or_die(config, "set -eq 0 -threads 1");
  error_code_t status =
      infer_for_test(config, 0, 0, 1, "ACBYEYY", "", "", inference_results);
  assert(status == ERROR_STATUS_INFERENCE_TARGET_LETTERS_NOT_IN_BAG);

  inference_results_destroy(inference_results);
  config_destroy(config);
}

void test_infer_empty_game_history(void) {
  Config *config =
      config_create_or_die("set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 "
                           "all -r2 all -numplays 1");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  InferenceResults *inference_results = inference_results_create(NULL);
  load_and_exec_config_or_die(config, "set -eq 0 -threads 1");
  error_code_t status =
      infer_for_test_with_history(config, inference_results, 0);
  assert(status == ERROR_STATUS_INFERENCE_EMPTY_GAME_HISTORY);
  inference_results_destroy(inference_results);
  config_destroy(config);
}

void test_infer_nonerror_cases(const int number_of_threads,
                               const bool use_game_history) {
  char *config_settings_str = get_formatted_string(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 "
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
  XoshiroPRNG *prng = prng_create(0);

  const Player *player0 = game_get_player(game, 0);
  const Player *player1 = game_get_player(game, 1);
  const KLV *klv = player_get_klv(player0);

  const Rack *player0_rack = player_get_rack(player0);
  const Rack *player1_rack = player_get_rack(player1);

  InferenceResults *inference_results = inference_results_create(NULL);
  Stat *letter_stat = stat_create(false);
  error_code_t status;
  const LeaveRackList *lrl;
  const char *gcg_string_header = "#character-encoding UTF-8\n#player1 Tim Tim "
                                  "Weiss\n#player2 Josh Josh Castellano\n";

  load_and_exec_config_or_die(config, "set -numplays 20");
  if (use_game_history) {
    load_game_history_with_gcg_string(config, gcg_string_header,
                                      ">Tim: MUZAKS 8H MUZAKS +52 52");
    status = infer_for_test_with_history(config, inference_results, 1);
  } else {
    status =
        infer_for_test(config, 0, 52, 0, "MUZAKS", "", "", inference_results);
  }
  assert(status == ERROR_STATUS_SUCCESS);
  // With this rack, only keeping an S is possible, and
  // there are 3 S remaining.

  const Stat *equity_values = inference_results_get_equity_values(
      inference_results, INFERENCE_TYPE_LEAVE);
  assert(stat_get_num_samples(equity_values) == 3);
  assert(stat_get_num_unique_samples(equity_values) == 1);
  rack_set_to_string(ld, rack, "S");
  assert(double_to_equity(stat_get_mean(equity_values)) ==
         klv_get_leave_value(klv, rack));
  AliasMethod *alias_method =
      inference_results_get_alias_method(inference_results);
  for (int i = 0; i < 100; i++) {
    assert(alias_method_sample(alias_method, prng, rack));
    assert(rack_get_total_letters(rack) == 1);
    assert(rack_get_letter(rack, ld_hl_to_ml(ld, "S")) == 1);
  }
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
  assert(leave_rack_get_leave_total_letters(leave_rack_list_get_rack(lrl, 0),
                                            ld) == 1);
  assert(leave_rack_get_leave_letter(leave_rack_list_get_rack(lrl, 0),
                                     ld_hl_to_ml(ld, "S"), ld) == 1);

  // Both game racks should be empty
  assert(rack_get_total_letters(player0_rack) == 0);
  assert(rack_get_total_letters(player1_rack) == 0);

  load_and_exec_config_or_die(config, "set -numplays 20");
  if (use_game_history) {
    load_game_history_with_gcg_string(config, gcg_string_header,
                                      ">Tim: MUZAKY 8H MUZAKY +58 58");
    status = infer_for_test_with_history(config, inference_results, 1);
  } else {
    status =
        infer_for_test(config, 0, 58, 0, "MUZAKY", "", "", inference_results);
  }

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
  alias_method = inference_results_get_alias_method(inference_results);
  for (int i = 0; i < 100; i++) {
    assert(alias_method_sample(alias_method, prng, rack));
    assert(rack_get_total_letters(rack) == 1);
  }
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
  assert(leave_rack_get_leave_total_letters(leave_rack_list_get_rack(lrl, 0),
                                            ld) == 1);
  assert(leave_rack_get_leave_letter(leave_rack_list_get_rack(lrl, 0),
                                     ld_hl_to_ml(ld, "E"), ld) == 1);
  assert(leave_rack_get_leave_letter(leave_rack_list_get_rack(lrl, 1),
                                     ld_hl_to_ml(ld, "I"), ld) == 1);

  // Both game racks should be empty
  assert(rack_get_total_letters(player0_rack) == 0);
  assert(rack_get_total_letters(player1_rack) == 0);

  if (use_game_history) {
    load_game_history_with_gcg_string(config, gcg_string_header,
                                      ">Tim: MUZAK 8H MUZAK +50 50");
    status = infer_for_test_with_history(config, inference_results, 1);
  } else {
    status =
        infer_for_test(config, 0, 50, 0, "MUZAK", "", "", inference_results);
  }

  assert(status == ERROR_STATUS_SUCCESS);
  alias_method = inference_results_get_alias_method(inference_results);
  for (int i = 0; i < 100; i++) {
    assert(alias_method_sample(alias_method, prng, rack));
    assert(rack_get_total_letters(rack) == 2);
  }
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
  load_and_exec_config_or_die(config, "set -eq 10000");
  if (use_game_history) {
    load_game_history_with_gcg(config, "vs_jeremy");
    status = infer_for_test_with_history(
        config, inference_results,
        game_history_get_num_events(config_get_game_history(config)));
  } else {
    status = infer_for_test(config, 0, 32, 0, "DEW??", "", "AHIILR",
                            inference_results);
  }
  assert(status == ERROR_STATUS_SUCCESS);
  // Refetch equity values because the underlying
  // inference_results results were recreated
  equity_values = inference_results_get_equity_values(inference_results,
                                                      INFERENCE_TYPE_LEAVE);
  assert(stat_get_num_samples(equity_values) == 1);
  assert(stat_get_num_unique_samples(equity_values) == 1);
  rack_set_to_string(ld, rack, "DS");
  assert(double_to_equity(stat_get_mean(equity_values)) ==
         klv_get_leave_value(klv, rack));
  alias_method = inference_results_get_alias_method(inference_results);
  for (int i = 0; i < 100; i++) {
    assert(alias_method_sample(alias_method, prng, rack));
    assert(rack_get_total_letters(rack) == 2);
    assert(rack_get_letter(rack, ld_hl_to_ml(ld, "D")) == 1);
    assert(rack_get_letter(rack, ld_hl_to_ml(ld, "S")) == 1);
  }
  for (int i = 0; i < ld_size; i++) {
    if (i == ld_hl_to_ml(ld, "D") || i == ld_hl_to_ml(ld, "S")) {
      assert(inference_results_get_subtotal(inference_results,
                                            INFERENCE_TYPE_LEAVE, i, 1,
                                            INFERENCE_SUBTOTAL_DRAW) == 1);
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

  if (use_game_history) {
    // Test that using the game history works for an event in the middle of the
    // game history
    load_and_exec_config_or_die(config, "set -eq 0");
    load_game_history_with_gcg(config, "vs_jeremy");
    status = infer_for_test_with_history(config, inference_results, 11);
    assert(status == ERROR_STATUS_SUCCESS);
    assert(stat_get_num_samples(equity_values) == 7);
    assert(stat_get_num_unique_samples(equity_values) == 4);
    alias_method = inference_results_get_alias_method(inference_results);
    const char *possible_leave_strs[] = {"E", "O", "P", "U"};
    const int num_possible_leave_strs =
        sizeof(possible_leave_strs) / sizeof(possible_leave_strs[0]);
    for (int i = 0; i < 100; i++) {
      assert(alias_method_sample(alias_method, prng, rack));
      assert(rack_get_total_letters(rack) == 1);
      bool has_letter = false;
      for (int j = 0; j < num_possible_leave_strs; j++) {
        has_letter =
            has_letter ||
            rack_get_letter(rack, ld_hl_to_ml(ld, possible_leave_strs[j])) == 1;
      }
      assert(has_letter);
    }
    game_reset(game);
  }

  load_and_exec_config_or_die(config, "set -numplays 100 -eq 0");
  if (use_game_history) {
    load_game_history_with_gcg_string(config, gcg_string_header,
                                      ">Tim: ERNT 8G RENT +8 8");
    status = infer_for_test_with_history(config, inference_results, 1);
  } else {
    status = infer_for_test(config, 0, 8, 0, "ENRT", "", "", inference_results);
  }

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

  assert(leave_rack_get_leave_total_letters(leave_rack_list_get_rack(lrl, 0),
                                            ld) == 3);
  assert(leave_rack_get_leave_letter(leave_rack_list_get_rack(lrl, 0),
                                     ld_hl_to_ml(ld, "E"), ld) == 1);
  assert(leave_rack_get_leave_letter(leave_rack_list_get_rack(lrl, 0),
                                     ld_hl_to_ml(ld, "R"), ld) == 1);
  assert(leave_rack_get_leave_letter(leave_rack_list_get_rack(lrl, 0),
                                     ld_hl_to_ml(ld, "T"), ld) == 1);

  assert(leave_rack_get_leave_total_letters(leave_rack_list_get_rack(lrl, 1),
                                            ld) == 3);
  assert(leave_rack_get_leave_letter(leave_rack_list_get_rack(lrl, 1),
                                     ld_hl_to_ml(ld, "N"), ld) == 1);
  assert(leave_rack_get_leave_letter(leave_rack_list_get_rack(lrl, 1),
                                     ld_hl_to_ml(ld, "R"), ld) == 1);
  assert(leave_rack_get_leave_letter(leave_rack_list_get_rack(lrl, 1),
                                     ld_hl_to_ml(ld, "T"), ld) == 1);

  assert(leave_rack_get_leave_total_letters(leave_rack_list_get_rack(lrl, 2),
                                            ld) == 3);
  assert(leave_rack_get_leave_letter(leave_rack_list_get_rack(lrl, 2),
                                     ld_hl_to_ml(ld, "N"), ld) == 1);
  assert(leave_rack_get_leave_letter(leave_rack_list_get_rack(lrl, 2),
                                     ld_hl_to_ml(ld, "R"), ld) == 1);
  assert(leave_rack_get_leave_letter(leave_rack_list_get_rack(lrl, 2),
                                     BLANK_MACHINE_LETTER, ld) == 1);

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
  status = infer_for_test(config, 0, 50, 0, "IIII", "", "", inference_results);
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
  if (use_game_history) {
    load_game_history_with_gcg_string(config, gcg_string_header,
                                      ">Tim: MUZAKY 8H MUZAKY +58 58");
    status = infer_for_test_with_history(config, inference_results, 1);
  } else {
    status =
        infer_for_test(config, 0, 58, 0, "MUZAKY", "", "", inference_results);
  }

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

  load_and_exec_config_or_die(config, "set -eq 0");
  if (use_game_history) {
    StringBuilder *gcg_builder = string_builder_create();
    // The phony of IX* should make the X a known tile for the inference of
    // GRIND
    string_builder_add_string(gcg_builder, ">Tim: I? 8H Iz +2 2\n");
    string_builder_add_string(gcg_builder, ">Tim: I? -- -2 0\n");
    string_builder_add_string(gcg_builder, ">Josh: AAAAAAA -AAAAAAA +0 0\n");
    string_builder_add_string(gcg_builder, ">Tim: GRIND 8D GRIND +18 18\n");
    load_game_history_with_gcg_string(config, gcg_string_header,
                                      string_builder_peek(gcg_builder));
    string_builder_destroy(gcg_builder);
    status = infer_for_test_with_history(config, inference_results, 4);
  } else {
    status =
        infer_for_test(config, 0, 18, 0, "GRIND", "?", "", inference_results);
  }

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

  load_and_exec_config_or_die(config, "set -eq 0");
  if (use_game_history) {
    StringBuilder *gcg_builder = string_builder_create();
    // The phony of IH* should make the H a known tile for the inference of
    // RIN
    string_builder_add_string(gcg_builder, ">Tim: IH 8H IH +10 10\n");
    string_builder_add_string(gcg_builder, ">Tim: IH -- -10 0\n");
    string_builder_add_string(gcg_builder, ">Josh: AAAAAAA -AAAAAAA +0 0\n");
    string_builder_add_string(gcg_builder, ">Tim: RIN 8G RIN +6 6\n");
    load_game_history_with_gcg_string(config, gcg_string_header,
                                      string_builder_peek(gcg_builder));
    string_builder_destroy(gcg_builder);
    status = infer_for_test_with_history(config, inference_results, 4);
  } else {
    status = infer_for_test(config, 0, 6, 0, "RIN", "H", "", inference_results);
  }

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

  rack_reset(rack);
  load_and_exec_config_or_die(config, "set -eq 0");
  if (use_game_history) {
    GameHistory *game_history = config_get_game_history(config);
    test_parse_gcg("exchange_with_seven_in_bag", config, game_history);
    status = infer_for_test_with_history(
        config, inference_results,
        game_history_get_num_events(config_get_game_history(config)));
    assert(status == ERROR_STATUS_SUCCESS);
    assert(stat_get_num_samples(equity_values) == 10);
    assert(stat_get_num_unique_samples(equity_values) == 8);
    // Keeping any one of H, N, or R is valid
    assert(inference_results_get_subtotal(
               inference_results, INFERENCE_TYPE_LEAVE, ld_hl_to_ml(ld, "H"), 1,
               INFERENCE_SUBTOTAL_DRAW) != 0);
    assert(inference_results_get_subtotal(
               inference_results, INFERENCE_TYPE_LEAVE, ld_hl_to_ml(ld, "N"), 1,
               INFERENCE_SUBTOTAL_DRAW) != 0);
    assert(inference_results_get_subtotal(
               inference_results, INFERENCE_TYPE_LEAVE, ld_hl_to_ml(ld, "R"), 1,
               INFERENCE_SUBTOTAL_DRAW) != 0);
  } else {
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

    status = infer_for_test(config, 0, 0, 6, "", "", "", inference_results);

    assert(status == ERROR_STATUS_SUCCESS);
    // Keeping any one of D, H, R, or S is valid
    assert(inference_results_get_subtotal(
               inference_results, INFERENCE_TYPE_LEAVE, ld_hl_to_ml(ld, "D"), 1,
               INFERENCE_SUBTOTAL_DRAW) != 0);
    assert(inference_results_get_subtotal(
               inference_results, INFERENCE_TYPE_LEAVE, ld_hl_to_ml(ld, "H"), 1,
               INFERENCE_SUBTOTAL_DRAW) != 0);
    assert(inference_results_get_subtotal(
               inference_results, INFERENCE_TYPE_LEAVE, ld_hl_to_ml(ld, "R"), 1,
               INFERENCE_SUBTOTAL_DRAW) != 0);
    assert(inference_results_get_subtotal(
               inference_results, INFERENCE_TYPE_LEAVE, ld_hl_to_ml(ld, "S"), 1,
               INFERENCE_SUBTOTAL_DRAW) != 0);

    // There are exchanges where throwing back at least one
    // of these is correct
    assert(inference_results_get_subtotal(
               inference_results, INFERENCE_TYPE_EXCHANGED,
               ld_hl_to_ml(ld, "D"), 1, INFERENCE_SUBTOTAL_DRAW) != 0);
    assert(inference_results_get_subtotal(
               inference_results, INFERENCE_TYPE_EXCHANGED,
               ld_hl_to_ml(ld, "L"), 1, INFERENCE_SUBTOTAL_DRAW) != 0);
    assert(inference_results_get_subtotal(
               inference_results, INFERENCE_TYPE_EXCHANGED,
               ld_hl_to_ml(ld, "Q"), 1, INFERENCE_SUBTOTAL_DRAW) != 0);
    assert(inference_results_get_subtotal(
               inference_results, INFERENCE_TYPE_EXCHANGED,
               ld_hl_to_ml(ld, "V"), 1, INFERENCE_SUBTOTAL_DRAW) != 0);
    assert(inference_results_get_subtotal(
               inference_results, INFERENCE_TYPE_EXCHANGED,
               ld_hl_to_ml(ld, "W"), 1, INFERENCE_SUBTOTAL_DRAW) != 0);

    // Exchanges with the I are never correct
    assert(inference_results_get_subtotal(
               inference_results, INFERENCE_TYPE_LEAVE, ld_hl_to_ml(ld, "I"), 1,
               INFERENCE_SUBTOTAL_DRAW) == 0);
    assert(inference_results_get_subtotal(
               inference_results, INFERENCE_TYPE_EXCHANGED,
               ld_hl_to_ml(ld, "I"), 1, INFERENCE_SUBTOTAL_DRAW) == 0);
  }

  game_reset(game);

  stat_destroy(letter_stat);
  rack_destroy(rack);
  inference_results_destroy(inference_results);
  prng_destroy(prng);
  config_destroy(config);
}

// Context for printing diagnostic info on failure
typedef struct InferDiagnostics {
  const char *cgp;
  const char *played_tiles_str;
  int game_num;
  int player_on_turn;
  int score;
  int target_num_exch;
  uint64_t seed;
} InferDiagnostics;

static void print_infer_diagnostics(const InferDiagnostics *diag) {
  printf("\n=== INFERENCE MISMATCH - REPRODUCTION INFO ===\n");
  printf("Game #: %d\n", diag->game_num);
  printf("Seed (for bag): %llu\n", (unsigned long long)diag->seed);
  printf("CGP: %s\n", diag->cgp);
  printf("Move played tiles (for scoring plays): %s\n", diag->played_tiles_str);
  printf("Target player index: %d\n", diag->player_on_turn);
  printf("Move score: %d\n", diag->score);
  printf("Tiles exchanged (0 for scoring plays): %d\n", diag->target_num_exch);
  printf("==============================================\n\n");
  fflush(stdout);
}

// Helper to compare inference results for correctness.
// Only compares integer counts (samples, unique_samples, subtotals) since
// mean/stdev can differ slightly due to floating-point accumulation order
// when running multithreaded.
static void assert_inference_results_equal(InferenceResults *results1,
                                           InferenceResults *results2,
                                           int ld_size,
                                           const InferDiagnostics *diag) {
  const Stat *equity1 =
      inference_results_get_equity_values(results1, INFERENCE_TYPE_LEAVE);
  const Stat *equity2 =
      inference_results_get_equity_values(results2, INFERENCE_TYPE_LEAVE);

  bool samples_match = true;
  if (stat_get_num_samples(equity1) != stat_get_num_samples(equity2)) {
    print_infer_diagnostics(diag);
    printf("num_samples mismatch: %llu vs %llu (diff: %lld)\n",
           (unsigned long long)stat_get_num_samples(equity1),
           (unsigned long long)stat_get_num_samples(equity2),
           (long long)(stat_get_num_samples(equity1) - stat_get_num_samples(equity2)));
    samples_match = false;
    // Don't assert yet - continue to find which letters differ
  }

  if (stat_get_num_unique_samples(equity1) != stat_get_num_unique_samples(equity2)) {
    if (samples_match) print_infer_diagnostics(diag);
    printf("num_unique_samples mismatch: %llu vs %llu (diff: %lld)\n",
           (unsigned long long)stat_get_num_unique_samples(equity1),
           (unsigned long long)stat_get_num_unique_samples(equity2),
           (long long)(stat_get_num_unique_samples(equity1) - stat_get_num_unique_samples(equity2)));
    samples_match = false;
  }

  // Skip mean/stdev comparison - these are derived from floating-point
  // accumulation which can vary with thread execution order.

  for (int i = 0; i < ld_size; i++) {
    for (int count = 1; count <= RACK_SIZE; count++) {
      uint64_t draw1 = inference_results_get_subtotal(results1, INFERENCE_TYPE_LEAVE, i,
                                            count, INFERENCE_SUBTOTAL_DRAW);
      uint64_t draw2 = inference_results_get_subtotal(results2, INFERENCE_TYPE_LEAVE, i,
                                            count, INFERENCE_SUBTOTAL_DRAW);
      if (draw1 != draw2) {
        printf("draw subtotal mismatch at letter %d count %d: %llu vs %llu (diff: %lld)\n",
               i, count, (unsigned long long)draw1, (unsigned long long)draw2,
               (long long)(draw1 - draw2));
        samples_match = false;
      }

      uint64_t leave1 = inference_results_get_subtotal(results1, INFERENCE_TYPE_LEAVE, i,
                                            count, INFERENCE_SUBTOTAL_LEAVE);
      uint64_t leave2 = inference_results_get_subtotal(results2, INFERENCE_TYPE_LEAVE, i,
                                            count, INFERENCE_SUBTOTAL_LEAVE);
      if (leave1 != leave2) {
        printf("leave subtotal mismatch at letter %d count %d: %llu vs %llu (diff: %lld)\n",
               i, count, (unsigned long long)leave1, (unsigned long long)leave2,
               (long long)(leave1 - leave2));
        samples_match = false;
      }
    }
  }

  // Now assert at the end after printing all differences
  assert(samples_match);
}

// Extract played tiles from a move into a Rack (handles blanks correctly)
static void get_played_tiles_from_move(const Move *move, Rack *played_tiles,
                                       int ld_size) {
  rack_set_dist_size_and_reset(played_tiles, ld_size);
  const game_event_t move_type = move_get_type(move);

  if (move_type == GAME_EVENT_TILE_PLACEMENT_MOVE) {
    const int tiles_length = move_get_tiles_length(move);
    for (int i = 0; i < tiles_length; i++) {
      uint8_t ml = move_get_tile(move, i);
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
    const int tiles_played = move_get_tiles_played(move);
    for (int i = 0; i < tiles_played; i++) {
      rack_add_letter(played_tiles, move_get_tile(move, i));
    }
  }
}

// Targeted test for a specific CGP/move combination
static void test_infer_cutoff_single_case(const char *cgp_str,
                                          const char *played_tiles_str,
                                          int player_on_turn, int score,
                                          int target_num_exch) {
  // Use single thread to eliminate floating-point accumulation order differences
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp false -s1 equity -s2 equity "
      "-r1 all -r2 all -numplays 1 -threads 1");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);

  Game *game = config_get_game(config);
  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);

  // Load the specific CGP
  load_cgp_or_die(game, cgp_str);

  InferenceResults *results_with_cutoff = inference_results_create(NULL);
  InferenceResults *results_without_cutoff = inference_results_create(NULL);

  // Run inference with cutoff optimization
  config_set_use_infer_cutoff_optimization(config, true);
  error_code_t status1 =
      infer_for_test(config, player_on_turn, score, target_num_exch,
                     played_tiles_str, "", "", results_with_cutoff);

  // Run inference without cutoff optimization
  config_set_use_infer_cutoff_optimization(config, false);
  error_code_t status2 =
      infer_for_test(config, player_on_turn, score, target_num_exch,
                     played_tiles_str, "", "", results_without_cutoff);

  printf("Status: %d vs %d\n", status1, status2);
  assert(status1 == status2);

  if (status1 == ERROR_STATUS_SUCCESS) {
    const Stat *equity1 =
        inference_results_get_equity_values(results_with_cutoff,
                                            INFERENCE_TYPE_LEAVE);
    const Stat *equity2 =
        inference_results_get_equity_values(results_without_cutoff,
                                            INFERENCE_TYPE_LEAVE);

    printf("With cutoff:    samples=%llu unique=%llu mean=%.17g stdev=%.17g\n",
           (unsigned long long)stat_get_num_samples(equity1),
           (unsigned long long)stat_get_num_unique_samples(equity1),
           stat_get_mean(equity1), stat_get_stdev(equity1));
    printf("Without cutoff: samples=%llu unique=%llu mean=%.17g stdev=%.17g\n",
           (unsigned long long)stat_get_num_samples(equity2),
           (unsigned long long)stat_get_num_unique_samples(equity2),
           stat_get_mean(equity2), stat_get_stdev(equity2));

    printf("Equity conversion: mean %d vs %d, stdev %d vs %d\n",
           double_to_equity(stat_get_mean(equity1)),
           double_to_equity(stat_get_mean(equity2)),
           double_to_equity(stat_get_stdev(equity1)),
           double_to_equity(stat_get_stdev(equity2)));

    InferDiagnostics diag = {
        .cgp = cgp_str,
        .played_tiles_str = played_tiles_str,
        .game_num = 0,
        .player_on_turn = player_on_turn,
        .score = score,
        .target_num_exch = target_num_exch,
        .seed = 0,
    };
    assert_inference_results_equal(results_with_cutoff, results_without_cutoff,
                                   ld_size, &diag);
  }

  inference_results_destroy(results_with_cutoff);
  inference_results_destroy(results_without_cutoff);
  config_destroy(config);
}

// Test the specific failing case from game 1 (seed 13345)
void test_infer_cutoff_repro(void) {
  // Game #: 1
  // Seed: 13345
  // CGP: 10ALPHA/9MM4/8QUIST2/11P3/10XI3/10UG3/11O3/6FOB2T3/7HENDS3/15/15/15/15/15/15 CDNVVZ?/EEILRRR 93/174 0
  // Played tiles: CDNVV (Exchanged)
  // Player on turn: 1
  // Score: 0
  // Tiles exchanged: 5
  test_infer_cutoff_single_case(
      "10ALPHA/9MM4/8QUIST2/11P3/10XI3/10UG3/11O3/6FOB2T3/7HENDS3/15/15/15/15/15/15 CDNVVZ?/EEILRRR 93/174 0",
      "", 1, 0, 5);
}

// Helper to dump leaves to file for diffing
static void dump_leaves_to_file(InferenceResults *results, const LetterDistribution *ld,
                                 const char *filename) {
  FILE *f = fopen(filename, "w");
  if (!f) return;

  LeaveRackList *lrl = inference_results_get_leave_rack_list(results);
  leave_rack_list_sort(lrl);
  int count = leave_rack_list_get_count(lrl);
  int ld_size = ld_get_size(ld);
  Rack leave, exchanged;
  rack_set_dist_size(&leave, ld_size);
  rack_set_dist_size(&exchanged, ld_size);

  StringBuilder *sb = string_builder_create();

  for (int i = 0; i < count; i++) {
    const LeaveRack *lr = leave_rack_list_get_rack(lrl, i);
    leave_rack_get_leave(lr, &leave);
    leave_rack_get_exchanged(lr, &exchanged);

    string_builder_clear(sb);
    string_builder_add_rack(sb, &leave, ld, false);
    const char *leave_str = string_builder_peek(sb);

    StringBuilder *sb2 = string_builder_create();
    string_builder_add_rack(sb2, &exchanged, ld, false);
    const char *exch_str = string_builder_peek(sb2);

    // Only output rack and draws - equity can differ slightly
    fprintf(f, "%s+%s %d\n", leave_str, exch_str, leave_rack_get_draws(lr));

    string_builder_destroy(sb2);
  }
  string_builder_destroy(sb);
  fclose(f);
}

// Test the failing case from 50-game WMP test (game #24, seed 37345)
void test_infer_cutoff_repro_wmp(void) {
  // Game #: 24
  // Seed (for bag): 37345
  // CGP: 15/15/15/15/4T10/4O10/2GENOA8/1ROSEOLA7/1A2T10/1I2I10/1N2C10/1D13/1A13/1T13/MEOW11 AAEEEEU/BNNOTUU 95/141 0
  // Move played tiles (for scoring plays): AEEEU
  // Target player index: 0
  // Move score: 0
  // Tiles exchanged (0 for scoring plays): 5

  const char *cgp_str = "15/15/15/15/4T10/4O10/2GENOA8/1ROSEOLA7/1A2T10/1I2I10/1N2C10/1D13/1A13/1T13/MEOW11 AAEEEEU/BNNOTUU 95/141 0";

  // Use WMP true to reproduce the bug
  // Use large numplays to store all unique leaves (we expect ~10,000)
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity "
      "-r1 all -r2 all -numplays 1000000 -threads 1");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);

  Game *game = config_get_game(config);
  const LetterDistribution *ld = game_get_ld(game);

  // Load the specific CGP
  load_cgp_or_die(game, cgp_str);

  InferenceResults *results_with_cutoff = inference_results_create(NULL);
  InferenceResults *results_without_cutoff = inference_results_create(NULL);

  // Run inference with cutoff optimization
  config_set_use_infer_cutoff_optimization(config, true);
  error_code_t status1 =
      infer_for_test(config, 0, 0, 5, "", "", "", results_with_cutoff);

  // Run inference without cutoff optimization
  config_set_use_infer_cutoff_optimization(config, false);
  error_code_t status2 =
      infer_for_test(config, 0, 0, 5, "", "", "", results_without_cutoff);

  printf("Status: %d vs %d\n", status1, status2);
  assert(status1 == status2);

  const Stat *equity1 =
      inference_results_get_equity_values(results_with_cutoff, INFERENCE_TYPE_LEAVE);
  const Stat *equity2 =
      inference_results_get_equity_values(results_without_cutoff, INFERENCE_TYPE_LEAVE);

  // Assert all unique leaves are captured in the leave rack lists
  LeaveRackList *lrl1 = inference_results_get_leave_rack_list(results_with_cutoff);
  LeaveRackList *lrl2 = inference_results_get_leave_rack_list(results_without_cutoff);
  int count1 = leave_rack_list_get_count(lrl1);
  int count2 = leave_rack_list_get_count(lrl2);
  int unique1 = (int)stat_get_num_unique_samples(equity1);
  int unique2 = (int)stat_get_num_unique_samples(equity2);

  printf("With cutoff:    count=%d unique=%d samples=%llu\n",
         count1, unique1, (unsigned long long)stat_get_num_samples(equity1));
  printf("Without cutoff: count=%d unique=%d samples=%llu\n",
         count2, unique2, (unsigned long long)stat_get_num_samples(equity2));

  // Ensure we captured all unique leaves (capacity was large enough)
  assert(count1 == unique1 && "capacity too small - not all unique leaves captured (with cutoff)");
  assert(count2 == unique2 && "capacity too small - not all unique leaves captured (without cutoff)");

  // Dump leaves to files for diffing
  dump_leaves_to_file(results_with_cutoff, ld, "/tmp/leaves_with_cutoff.txt");
  dump_leaves_to_file(results_without_cutoff, ld, "/tmp/leaves_without_cutoff.txt");
  printf("Leaves dumped to /tmp/leaves_with_cutoff.txt and /tmp/leaves_without_cutoff.txt\n");
  printf("Run: diff /tmp/leaves_with_cutoff.txt /tmp/leaves_without_cutoff.txt\n");

  inference_results_destroy(results_with_cutoff);
  inference_results_destroy(results_without_cutoff);
  config_destroy(config);
}

// Test that the cutoff optimization produces identical results to the
// non-optimized version by playing random games and comparing inference
// timing for different play types.
void test_infer_cutoff_optimization_comparison(void) {
  const int NUM_GAMES = 500;   // 500 games, plays 1-6 and exchanges 1-7
  const int NUM_THREADS = 10;  // Full cores available

  // Use equity margin of 0 to test correctness (strictest test)
  // Use CSW21 with WMP for full WMP instrumentation
  char config_str[256];
  snprintf(config_str, sizeof(config_str),
           "set -lex CSW21 -wmp true -s1 equity -s2 equity "
           "-r1 all -r2 all -numplays 1 -threads %d", NUM_THREADS);
  Config *config = config_create_or_die(config_str);
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);

  Game *game = config_get_game(config);
  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);
  Bag *bag = game_get_bag(game);

  MoveList *move_list = move_list_create(1);

  InferenceResults *results_with_cutoff = inference_results_create(NULL);
  InferenceResults *results_without_cutoff = inference_results_create(NULL);

  // Track timing by tiles played (1-7) for scoring plays and exchanges
  double time_with_cutoff_play[RACK_SIZE + 1] = {0};
  double time_without_cutoff_play[RACK_SIZE + 1] = {0};
  int count_play[RACK_SIZE + 1] = {0};

  double time_with_cutoff_exch[RACK_SIZE + 1] = {0};
  double time_without_cutoff_exch[RACK_SIZE + 1] = {0};
  int count_exch[RACK_SIZE + 1] = {0};

  Timer timer;
  Rack played_tiles;
  rack_set_dist_size(&played_tiles, ld_size);

  // Counter for 7-tile exchanges found
  int num_seven_tile_exchanges_found = 0;

  printf("\nPlaying %d random games, inference for plays 1-6 and exchanges 1-7...\n",
         NUM_GAMES);
  fflush(stdout);

  // Reset stats before the test
  inference_reset_cutoff_stats();
  gen_reset_anchor_stats();
  gen_reset_early_cutoff_stats();
  gen_reset_wmp_subanchor_stats();

  // Track anchor stats separately for with/without optimization
  uint64_t anchors_available_with = 0, anchors_processed_with = 0;
  uint64_t anchors_skipped_with = 0;
  uint64_t anchors_available_without = 0, anchors_processed_without = 0;
  uint64_t anchors_skipped_without = 0;

  // Track subrack stats separately for with/without optimization
  uint64_t subracks_available_with = 0, subracks_processed_with = 0;
  uint64_t subracks_skipped_with = 0;
  uint64_t subracks_available_without = 0, subracks_processed_without = 0;
  uint64_t subracks_skipped_without = 0;

  // Track per-size stats for anchors (index 0-7 for play sizes 1-7 and exch 1-7)
  // play_anchors_with[i] = anchors processed for i-tile plays WITH cutoff
  uint64_t play_anchors_with[RACK_SIZE + 1] = {0};
  uint64_t play_anchors_without[RACK_SIZE + 1] = {0};
  uint64_t exch_anchors_with[RACK_SIZE + 1] = {0};
  uint64_t exch_anchors_without[RACK_SIZE + 1] = {0};
  // Track early cutoff potential per size
  uint64_t play_movegen_calls[RACK_SIZE + 1] = {0};
  uint64_t play_shadow_skippable[RACK_SIZE + 1] = {0};
  uint64_t play_anchor_filterable[RACK_SIZE + 1] = {0};
  uint64_t play_anchor_total[RACK_SIZE + 1] = {0};
  uint64_t exch_movegen_calls[RACK_SIZE + 1] = {0};
  uint64_t exch_shadow_skippable[RACK_SIZE + 1] = {0};
  uint64_t exch_anchor_filterable[RACK_SIZE + 1] = {0};
  uint64_t exch_anchor_total[RACK_SIZE + 1] = {0};

  for (int game_num = 0; game_num < NUM_GAMES; game_num++) {
    // Reset game with unique seed for each game
    game_reset(game);
    // Start from game #1 (seed 13345) to debug the failing case
    bag_seed(bag, 13345 + (uint64_t)game_num * 1000);
    draw_starting_racks(game);

    // Play until game over
    while (!game_over(game)) {
      const int player_on_turn = game_get_player_on_turn_index(game);
      const int tiles_in_bag = bag_get_letters(bag);

      // Generate best move
      const Move *move = get_top_equity_move(game, 0, move_list);
      const game_event_t move_type = move_get_type(move);
      const int tiles_played = move_get_tiles_played(move);
      const int score = equity_to_int(move_get_score(move));

      // Skip passes
      if (move_type == GAME_EVENT_PASS) {
        play_move(move, game, NULL);
        continue;
      }

      // Get the played tiles for inference
      get_played_tiles_from_move(move, &played_tiles, ld_size);
      const int num_tiles_in_rack = rack_get_total_letters(&played_tiles);

      // Only infer if there are enough tiles in bag (before playing move)
      if (tiles_in_bag < RACK_SIZE) {
        play_move(move, game, NULL);
        continue;
      }

      // FILTER: Run inference for plays 1-6 and exchanges 1-7
      const bool is_scoring_play_1_to_6 =
          (move_type == GAME_EVENT_TILE_PLACEMENT_MOVE) &&
          (tiles_played >= 1 && tiles_played <= 6);
      const bool is_exchange_1_to_7 =
          (move_type == GAME_EVENT_EXCHANGE) &&
          (tiles_played >= 1 && tiles_played <= 7);
      if (!is_scoring_play_1_to_6 && !is_exchange_1_to_7) {
        play_move(move, game, NULL);
        continue;
      }
      if (is_exchange_1_to_7 && tiles_played == 7) {
        num_seven_tile_exchanges_found++;
      }

      // Build played tiles string for inference using StringBuilder
      StringBuilder *sb = string_builder_create();
      string_builder_add_rack(sb, &played_tiles, ld, false);
      char *played_tiles_str = string_builder_dump(sb, NULL);
      string_builder_destroy(sb);

      const int target_num_exch =
          (move_type == GAME_EVENT_EXCHANGE) ? tiles_played : 0;

      // Capture the CGP before running inference (for diagnostics)
      char *cgp = game_get_cgp(game, true);
      const uint64_t seed = 13345 + (uint64_t)game_num * 1000;


      // For exchanges, played_tiles should be empty
      const char *tiles_for_infer =
          (move_type == GAME_EVENT_EXCHANGE) ? "" : played_tiles_str;

      // Reset results before each inference to get per-move comparison
      inference_results_reset(results_with_cutoff, 1, ld_size);
      inference_results_reset(results_without_cutoff, 1, ld_size);

      // Run inference with cutoff optimization
      config_set_use_infer_cutoff_optimization(config, true);
      gen_reset_anchor_stats();
      gen_reset_subrack_stats();
      gen_reset_early_cutoff_stats();
      ctimer_start(&timer);
      error_code_t status1 =
          infer_for_test(config, player_on_turn, score, target_num_exch,
                         tiles_for_infer, "", "", results_with_cutoff);
      double time_with = ctimer_elapsed_seconds(&timer);
      {
        uint64_t avail, proc, skip;
        gen_get_anchor_stats(&avail, &proc, &skip);
        anchors_available_with += avail;
        anchors_processed_with += proc;
        anchors_skipped_with += skip;
        gen_get_subrack_stats(&avail, &proc, &skip);
        subracks_available_with += avail;
        subracks_processed_with += proc;
        subracks_skipped_with += skip;
        // Per-size anchor tracking
        if (move_type == GAME_EVENT_EXCHANGE) {
          exch_anchors_with[tiles_played] += proc;
        } else {
          play_anchors_with[tiles_played] += proc;
        }
        // Per-size early cutoff potential
        uint64_t mg_calls, shadow_skip, anch_filt, anch_tot;
        gen_get_early_cutoff_stats(&mg_calls, &shadow_skip, &anch_filt, &anch_tot);
        if (move_type == GAME_EVENT_EXCHANGE) {
          exch_movegen_calls[tiles_played] += mg_calls;
          exch_shadow_skippable[tiles_played] += shadow_skip;
          exch_anchor_filterable[tiles_played] += anch_filt;
          exch_anchor_total[tiles_played] += anch_tot;
        } else {
          play_movegen_calls[tiles_played] += mg_calls;
          play_shadow_skippable[tiles_played] += shadow_skip;
          play_anchor_filterable[tiles_played] += anch_filt;
          play_anchor_total[tiles_played] += anch_tot;
        }
      }

      // Run inference without cutoff optimization
      config_set_use_infer_cutoff_optimization(config, false);
      gen_reset_anchor_stats();
      gen_reset_subrack_stats();
      ctimer_start(&timer);
      error_code_t status2 =
          infer_for_test(config, player_on_turn, score, target_num_exch,
                         tiles_for_infer, "", "", results_without_cutoff);
      double time_without = ctimer_elapsed_seconds(&timer);
      {
        uint64_t avail, proc, skip;
        gen_get_anchor_stats(&avail, &proc, &skip);
        anchors_available_without += avail;
        anchors_processed_without += proc;
        anchors_skipped_without += skip;
        gen_get_subrack_stats(&avail, &proc, &skip);
        subracks_available_without += avail;
        subracks_processed_without += proc;
        subracks_skipped_without += skip;
        // Per-size anchor tracking
        if (move_type == GAME_EVENT_EXCHANGE) {
          exch_anchors_without[tiles_played] += proc;
        } else {
          play_anchors_without[tiles_played] += proc;
        }
      }

      // Now play the move on the main game to continue
      play_move(move, game, NULL);

      // Both should succeed or both should fail
      assert(status1 == status2);

      if (status1 == ERROR_STATUS_SUCCESS) {
        // Prepare diagnostics for potential failure
        InferDiagnostics diag = {
            .cgp = cgp,
            .played_tiles_str = played_tiles_str,
            .game_num = game_num,
            .player_on_turn = player_on_turn,
            .score = score,
            .target_num_exch = target_num_exch,
            .seed = seed,
        };

        // Verify results are identical
        assert_inference_results_equal(results_with_cutoff,
                                       results_without_cutoff, ld_size, &diag);

        // Track timing by play type
        if (move_type == GAME_EVENT_EXCHANGE) {
          time_with_cutoff_exch[num_tiles_in_rack] += time_with;
          time_without_cutoff_exch[num_tiles_in_rack] += time_without;
          count_exch[num_tiles_in_rack]++;
        } else {
          time_with_cutoff_play[num_tiles_in_rack] += time_with;
          time_without_cutoff_play[num_tiles_in_rack] += time_without;
          count_play[num_tiles_in_rack]++;
        }
      }

      free(cgp);
      free(played_tiles_str);
    }

    if ((game_num + 1) % 100 == 0) {
      printf("  Completed %d games, found %d 7-tile exchanges so far...\n",
             game_num + 1, num_seven_tile_exchanges_found);
      fflush(stdout);
    }
  }

  // Print results
  printf("\n=== 7-Tile Exchange Search Results ===\n");
  printf("Found %d 7-tile exchanges in %d games\n\n",
         num_seven_tile_exchanges_found, NUM_GAMES);
  printf("=== Inference Cutoff Optimization Results ===\n");
  printf("\nScoring plays (tiles played -> count, with cutoff, without cutoff, "
         "speedup):\n");
  double total_with_play = 0, total_without_play = 0;
  int total_count_play = 0;
  for (int i = 1; i <= RACK_SIZE; i++) {
    if (count_play[i] > 0) {
      double speedup = time_without_cutoff_play[i] / time_with_cutoff_play[i];
      printf("  %d tiles: %5d plays, %.4fs with (%.4fs/inf), %.4fs without "
             "(%.4fs/inf), %.2fx speedup\n",
             i, count_play[i], time_with_cutoff_play[i],
             time_with_cutoff_play[i] / count_play[i],
             time_without_cutoff_play[i],
             time_without_cutoff_play[i] / count_play[i], speedup);
      total_with_play += time_with_cutoff_play[i];
      total_without_play += time_without_cutoff_play[i];
      total_count_play += count_play[i];
    }
  }
  if (total_count_play > 0) {
    printf("  TOTAL:   %5d plays, %.4fs with (%.4fs/inf), %.4fs without "
           "(%.4fs/inf), %.2fx speedup\n",
           total_count_play, total_with_play,
           total_with_play / total_count_play, total_without_play,
           total_without_play / total_count_play,
           total_without_play / total_with_play);
  }

  printf("\nExchanges (tiles exchanged -> count, with cutoff, without cutoff, "
         "speedup):\n");
  double total_with_exch = 0, total_without_exch = 0;
  int total_count_exch = 0;
  for (int i = 1; i <= RACK_SIZE; i++) {
    if (count_exch[i] > 0) {
      double speedup = time_without_cutoff_exch[i] / time_with_cutoff_exch[i];
      printf("  %d tiles: %5d exchs, %.4fs with (%.4fs/inf), %.4fs without "
             "(%.4fs/inf), %.2fx speedup\n",
             i, count_exch[i], time_with_cutoff_exch[i],
             time_with_cutoff_exch[i] / count_exch[i],
             time_without_cutoff_exch[i],
             time_without_cutoff_exch[i] / count_exch[i], speedup);
      total_with_exch += time_with_cutoff_exch[i];
      total_without_exch += time_without_cutoff_exch[i];
      total_count_exch += count_exch[i];
    }
  }
  if (total_count_exch > 0) {
    printf("  TOTAL:   %5d exchs, %.4fs with (%.4fs/inf), %.4fs without "
           "(%.4fs/inf), %.2fx speedup\n",
           total_count_exch, total_with_exch,
           total_with_exch / total_count_exch, total_without_exch,
           total_without_exch / total_count_exch,
           total_without_exch / total_with_exch);
  }

  printf("\nOverall: %.4fs with cutoff, %.4fs without, %.2fx speedup\n",
         total_with_play + total_with_exch,
         total_without_play + total_without_exch,
         (total_without_play + total_without_exch) /
             (total_with_play + total_with_exch));

  // Print cutoff statistics
  printf("\n");
  inference_print_cutoff_stats();

  // Print anchor stats
  printf("\nAnchor stats WITH cutoff optimization:\n");
  {
    double skip_pct = anchors_available_with > 0 ?
        100.0 * (double)anchors_skipped_with / (double)anchors_available_with : 0;
    printf("  %llu processed / %llu available (%llu skipped, %.2f%%)\n",
           (unsigned long long)anchors_processed_with,
           (unsigned long long)anchors_available_with,
           (unsigned long long)anchors_skipped_with, skip_pct);
  }
  printf("Anchor stats WITHOUT cutoff optimization:\n");
  {
    double skip_pct = anchors_available_without > 0 ?
        100.0 * (double)anchors_skipped_without / (double)anchors_available_without : 0;
    printf("  %llu processed / %llu available (%llu skipped, %.2f%%)\n",
           (unsigned long long)anchors_processed_without,
           (unsigned long long)anchors_available_without,
           (unsigned long long)anchors_skipped_without, skip_pct);
  }

  // Print subrack stats
  printf("\nSubrack stats WITH cutoff optimization:\n");
  {
    double skip_pct = subracks_available_with > 0 ?
        100.0 * (double)subracks_skipped_with / (double)subracks_available_with : 0;
    printf("  %llu processed / %llu available (%llu skipped, %.2f%%)\n",
           (unsigned long long)subracks_processed_with,
           (unsigned long long)subracks_available_with,
           (unsigned long long)subracks_skipped_with, skip_pct);
  }
  printf("Subrack stats WITHOUT cutoff optimization:\n");
  {
    double skip_pct = subracks_available_without > 0 ?
        100.0 * (double)subracks_skipped_without / (double)subracks_available_without : 0;
    printf("  %llu processed / %llu available (%llu skipped, %.2f%%)\n",
           (unsigned long long)subracks_processed_without,
           (unsigned long long)subracks_available_without,
           (unsigned long long)subracks_skipped_without, skip_pct);
  }

  // Print early cutoff optimization potential (aggregate)
  {
    uint64_t tot_mg = 0, tot_shadow = 0, tot_filt = 0, tot_anch = 0;
    for (int i = 1; i <= RACK_SIZE; i++) {
      tot_mg += play_movegen_calls[i] + exch_movegen_calls[i];
      tot_shadow += play_shadow_skippable[i] + exch_shadow_skippable[i];
      tot_filt += play_anchor_filterable[i] + exch_anchor_filterable[i];
      tot_anch += play_anchor_total[i] + exch_anchor_total[i];
    }
    printf("\nEarly cutoff optimization potential (aggregate):\n");
    if (tot_mg > 0) {
      double shadow_skip_pct = 100.0 * (double)tot_shadow / (double)tot_mg;
      printf("  Shadow skippable by exchange: %llu / %llu (%.2f%%)\n",
             (unsigned long long)tot_shadow, (unsigned long long)tot_mg, shadow_skip_pct);
    }
    if (tot_anch > 0) {
      double anchor_filter_pct = 100.0 * (double)tot_filt / (double)tot_anch;
      printf("  Anchors filterable by shadow equity: %llu / %llu (%.2f%%)\n",
             (unsigned long long)tot_filt, (unsigned long long)tot_anch, anchor_filter_pct);
    }
  }

  // Print per-size anchor breakdown
  printf("\nPer-size anchor breakdown (tiles -> anchors with cutoff / without / ratio):\n");
  printf("Scoring plays:\n");
  for (int i = 1; i <= RACK_SIZE; i++) {
    if (play_anchors_with[i] > 0 || play_anchors_without[i] > 0) {
      double ratio = play_anchors_without[i] > 0 ?
          (double)play_anchors_with[i] / (double)play_anchors_without[i] : 0;
      // Compute hypothetical anchors if we skip shadow + filter by shadow equity
      uint64_t hypo_anchors = play_anchors_with[i];
      if (play_movegen_calls[i] > 0 && play_shadow_skippable[i] > 0) {
        // Shadow skip would eliminate all anchors for those calls
        double frac_skippable = (double)play_shadow_skippable[i] / (double)play_movegen_calls[i];
        hypo_anchors = (uint64_t)((1.0 - frac_skippable) * (double)play_anchors_with[i]);
      }
      // Further reduce by filterable anchors
      if (play_anchor_total[i] > 0 && play_anchor_filterable[i] > 0) {
        double frac_filt = (double)play_anchor_filterable[i] / (double)play_anchor_total[i];
        hypo_anchors = (uint64_t)((1.0 - frac_filt) * (double)hypo_anchors);
      }
      double hypo_ratio = play_anchors_without[i] > 0 ?
          (double)hypo_anchors / (double)play_anchors_without[i] : 0;
      printf("  %d tiles: %12llu / %12llu (%.2fx) -> hypo: %12llu (%.2fx)\n",
             i, (unsigned long long)play_anchors_with[i],
             (unsigned long long)play_anchors_without[i], ratio,
             (unsigned long long)hypo_anchors, hypo_ratio);
    }
  }
  printf("Exchanges:\n");
  for (int i = 1; i <= RACK_SIZE; i++) {
    if (exch_anchors_with[i] > 0 || exch_anchors_without[i] > 0) {
      double ratio = exch_anchors_without[i] > 0 ?
          (double)exch_anchors_with[i] / (double)exch_anchors_without[i] : 0;
      uint64_t hypo_anchors = exch_anchors_with[i];
      if (exch_movegen_calls[i] > 0 && exch_shadow_skippable[i] > 0) {
        double frac_skippable = (double)exch_shadow_skippable[i] / (double)exch_movegen_calls[i];
        hypo_anchors = (uint64_t)((1.0 - frac_skippable) * (double)exch_anchors_with[i]);
      }
      if (exch_anchor_total[i] > 0 && exch_anchor_filterable[i] > 0) {
        double frac_filt = (double)exch_anchor_filterable[i] / (double)exch_anchor_total[i];
        hypo_anchors = (uint64_t)((1.0 - frac_filt) * (double)hypo_anchors);
      }
      double hypo_ratio = exch_anchors_without[i] > 0 ?
          (double)hypo_anchors / (double)exch_anchors_without[i] : 0;
      printf("  %d tiles: %12llu / %12llu (%.2fx) -> hypo: %12llu (%.2fx)\n",
             i, (unsigned long long)exch_anchors_with[i],
             (unsigned long long)exch_anchors_without[i], ratio,
             (unsigned long long)hypo_anchors, hypo_ratio);
    }
  }

  // Print WMP subanchor statistics
  {
    uint64_t wmp_total, wmp_skippable;
    gen_get_wmp_subanchor_stats(&wmp_total, &wmp_skippable);
    if (wmp_total > 0) {
      printf("\nWMP subanchor skip analysis (instrumentation only, no actual filtering):\n");
      printf("  Total subanchors: %llu\n", (unsigned long long)wmp_total);
      printf("  Skippable (equity < cutoff): %llu (%.2f%%)\n",
             (unsigned long long)wmp_skippable,
             100.0 * (double)wmp_skippable / (double)wmp_total);
      gen_print_wmp_subanchor_breakdown();
    }
  }

  inference_results_destroy(results_with_cutoff);
  inference_results_destroy(results_without_cutoff);
  move_list_destroy(move_list);
  config_destroy(config);
}

void test_infer(void) {
  test_leave_rack_reset();
  test_trivial_random_probability();
  test_infer_rack_overflow();
  test_infer_no_tiles_played_rack_empty();
  test_infer_both_play_and_exchange();
  test_infer_exchange_score_not_zero();
  test_infer_exchange_not_board_is_letter_allowed_in_cross_set();
  test_infer_tiles_played_not_in_bag();
  test_infer_empty_game_history();

  for (int i = 0; i < 2; i++) {
    const bool use_game_history = i == 1;
    test_infer_nonerror_cases(1, use_game_history);
    test_infer_nonerror_cases(2, use_game_history);
    test_infer_nonerror_cases(7, use_game_history);
  }
  test_infer_cutoff_repro();
  test_infer_cutoff_repro_wmp();
  // test_infer_cutoff_optimization_comparison();  // Disabled - slow
}
