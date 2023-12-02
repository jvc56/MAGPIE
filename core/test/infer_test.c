#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "../src/game.h"
#include "../src/infer.h"
#include "../src/klv.h"
#include "../src/move.h"
#include "../src/thread_control.h"

#include "test_constants.h"
#include "test_util.h"
#include "testconfig.h"

inference_status_t infer_for_test(const Config *config, Game *game,
                                  Inference *inference) {
  inference_status_t status = infer(config, game, inference);
  return status;
}

void test_trivial_random_probability(TestConfig *testconfig) {
  const Config *config = get_csw_config(testconfig);
  Game *game = create_game(config);
  Inference *inference = create_inference();

  inference->bag_as_rack = create_rack(game->gen->letter_distribution->size);
  inference->leave = create_rack(game->gen->letter_distribution->size);

  // A minimum of zero should always be 100% probability
  assert(within_epsilon(get_probability_for_random_minimum_draw(
                            inference->bag_as_rack, inference->leave,
                            human_readable_letter_to_machine_letter(
                                game->gen->letter_distribution, "Z"),
                            0, 3),
                        1));
  assert(within_epsilon(get_probability_for_random_minimum_draw(
                            inference->bag_as_rack, inference->leave,
                            human_readable_letter_to_machine_letter(
                                game->gen->letter_distribution, "Z"),
                            0, 4),
                        1));
  assert(within_epsilon(get_probability_for_random_minimum_draw(
                            inference->bag_as_rack, inference->leave,
                            human_readable_letter_to_machine_letter(
                                game->gen->letter_distribution, "E"),
                            0, 6),
                        1));
  assert(within_epsilon(get_probability_for_random_minimum_draw(
                            inference->bag_as_rack, inference->leave,
                            human_readable_letter_to_machine_letter(
                                game->gen->letter_distribution, "E"),
                            -1, 4),
                        1));

  // Minimum N where letters in bag is M and M > N
  // should always be 0
  assert(within_epsilon(get_probability_for_random_minimum_draw(
                            inference->bag_as_rack, inference->leave,
                            human_readable_letter_to_machine_letter(
                                game->gen->letter_distribution, "E"),
                            20, 4),
                        0));

  // If the player is emptying the bag and there are the minimum
  // number of leaves remaining, the probability is trivially 1.
  add_letter_to_rack(inference->bag_as_rack,
                     human_readable_letter_to_machine_letter(
                         config->letter_distribution, "E"));
  add_letter_to_rack(inference->bag_as_rack,
                     human_readable_letter_to_machine_letter(
                         config->letter_distribution, "E"));
  add_letter_to_rack(inference->bag_as_rack,
                     human_readable_letter_to_machine_letter(
                         config->letter_distribution, "E"));
  add_letter_to_rack(inference->bag_as_rack,
                     human_readable_letter_to_machine_letter(
                         config->letter_distribution, "E"));
  add_letter_to_rack(inference->bag_as_rack,
                     human_readable_letter_to_machine_letter(
                         config->letter_distribution, "E"));
  assert(within_epsilon(get_probability_for_random_minimum_draw(
                            inference->bag_as_rack, inference->leave,
                            human_readable_letter_to_machine_letter(
                                game->gen->letter_distribution, "E"),
                            4, 1),
                        1));

  destroy_game(game);
  destroy_inference(inference);
}

void test_infer_rack_overflow(TestConfig *testconfig) {
  Config *config = get_csw_config(testconfig);
  Game *game = create_game(config);

  Inference *inference = create_inference();
  load_config_or_die(
      config,
      "setoptions rack ABCDEFGH pindex 0 score 0 exch 0 eq 0 threads 1");
  inference_status_t status = infer_for_test(config, game, inference);
  assert(status == INFERENCE_STATUS_RACK_OVERFLOW);
  reset_game(game);

  set_rack_to_string(game->gen->letter_distribution, game->players[0]->rack,
                     "ABC");
  load_config_or_die(
      config, "setoptions rack DEFGH pindex 0 score 0 exch 0 eq 0 threads 1");
  status = infer_for_test(config, game, inference);
  assert(status == INFERENCE_STATUS_RACK_OVERFLOW);

  destroy_inference(inference);
  destroy_game(game);
}

void test_infer_no_tiles_played_rack_empty(TestConfig *testconfig) {
  Config *config = get_csw_config(testconfig);
  Game *game = create_game(config);

  Inference *inference = create_inference();
  load_config_or_die(config, "setoptions rack " EMPTY_RACK_STRING
                             " pindex 0 score 0 exch 0 eq 0 threads 1");
  inference_status_t status = infer_for_test(config, game, inference);
  assert(status == INFERENCE_STATUS_NO_TILES_PLAYED);

  destroy_inference(inference);
  destroy_game(game);
}

void test_infer_no_tiles_played_rack_null(TestConfig *testconfig) {
  Config *config = get_csw_config(testconfig);
  Game *game = create_game(config);

  Inference *inference = create_inference();
  load_config_or_die(config,
                     "setoptions pindex 0 score 0 exch 0 eq 0 threads 1");
  inference_status_t status = infer_for_test(config, game, inference);
  assert(status == INFERENCE_STATUS_NO_TILES_PLAYED);

  destroy_inference(inference);
  destroy_game(game);
}

void test_infer_both_play_and_exchange(TestConfig *testconfig) {
  Config *config = get_csw_config(testconfig);
  Game *game = create_game(config);

  Inference *inference = create_inference();
  load_config_or_die(
      config, "setoptions rack DEFGH pindex 0 score 0 exch 1 eq 0 threads 1");
  inference_status_t status = infer_for_test(config, game, inference);
  assert(status == INFERENCE_STATUS_BOTH_PLAY_AND_EXCHANGE);

  destroy_inference(inference);
  destroy_game(game);
}

void test_infer_exchange_score_not_zero(TestConfig *testconfig) {
  Config *config = get_csw_config(testconfig);
  Game *game = create_game(config);

  Inference *inference = create_inference();
  load_config_or_die(config, "setoptions rack " EMPTY_RACK_STRING
                             " pindex 0 score 3 exch 1 eq 0 threads 1");
  inference_status_t status = infer_for_test(config, game, inference);
  assert(status == INFERENCE_STATUS_EXCHANGE_SCORE_NOT_ZERO);

  destroy_inference(inference);
  destroy_game(game);
}

void test_infer_exchange_not_allowed(TestConfig *testconfig) {
  Config *config = get_csw_config(testconfig);
  Game *game = create_game(config);

  // There are 13 tiles in the bag
  load_cgp(game, VS_JEREMY);
  Inference *inference = create_inference();
  load_config_or_die(config, "setoptions rack " EMPTY_RACK_STRING
                             " pindex 0 score 3 exch 1 eq 0 threads 1");
  inference_status_t status = infer_for_test(config, game, inference);
  assert(status == INFERENCE_STATUS_EXCHANGE_NOT_ALLOWED);

  add_letter(game->gen->bag, BLANK_MACHINE_LETTER, 0);
  // There should now be 14 tiles in the bag
  status = infer_for_test(config, game, inference);
  assert(status == INFERENCE_STATUS_EXCHANGE_SCORE_NOT_ZERO);

  destroy_inference(inference);
  destroy_game(game);
}

void test_infer_tiles_played_not_in_bag(TestConfig *testconfig) {
  Config *config = get_csw_config(testconfig);
  Game *game = create_game(config);

  Inference *inference = create_inference();
  load_config_or_die(
      config, "setoptions rack ABCYEYY pindex 0 score 0 exch 1 eq 0 threads 1");
  inference_status_t status = infer_for_test(config, game, inference);
  assert(status == INFERENCE_STATUS_TILES_PLAYED_NOT_IN_BAG);

  destroy_inference(inference);
  destroy_game(game);
}

void test_infer_nonerror_cases(TestConfig *testconfig, int number_of_threads) {
  Config *config = get_csw_config(testconfig);
  Game *game = create_game(config);
  Rack *rack = create_rack(game->players[0]->get_array_size(rack));
  Bag *bag = game->gen->bag;
  const KLV *klv = game->players[0]->klv;
  const LetterDistribution *ld = config->letter_distribution;
  Inference *inference = create_inference();
  Stat *letter_stat = create_stat();
  inference_status_t status;

  char *setoptions_thread_string =
      get_formatted_string("setoptions threads %d", number_of_threads);
  load_config_or_die(config, setoptions_thread_string);
  free(setoptions_thread_string);

  load_config_or_die(config,
                     "setoptions rack MUZAKS pindex 0 score 52 exch 0 eq 0");
  status = infer_for_test(config, game, inference);
  assert(status == INFERENCE_STATUS_SUCCESS);
  // With this rack, only keeping an S is possible, and
  // there are 3 S remaining.
  assert(get_weight(inference->leave_record->equity_values) == 3);
  assert(get_cardinality(inference->leave_record->equity_values) == 1);
  set_rack_to_string(ld, rack, "S");
  assert(within_epsilon(get_mean(inference->leave_record->equity_values),
                        get_leave_value(klv, rack)));
  for (uint32_t i = 0; i < ld->size; i++) {
    if (i == human_readable_letter_to_machine_letter(ld, "S")) {
      assert(get_subtotal(inference->leave_record, i, 1,
                          INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 3);
      assert(get_subtotal(inference->leave_record, i, 1,
                          INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE) == 1);
    } else {
      assert(get_subtotal(inference->leave_record, i, 1,
                          INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 0);
    }
  }
  get_stat_for_letter(inference->leave_record, letter_stat,
                      human_readable_letter_to_machine_letter(ld, "S"));
  assert(within_epsilon(get_mean(letter_stat), 1));
  assert(within_epsilon(get_stdev(letter_stat), 0));
  assert(within_epsilon(get_probability_for_random_minimum_draw(
                            inference->bag_as_rack, inference->leave,
                            human_readable_letter_to_machine_letter(ld, "S"), 1,
                            6),
                        (double)3 / 94));
  // Both game racks should be empty
  assert(game->players[0]->rack->number_of_letters == 0);
  assert(game->players[1]->rack->number_of_letters == 0);

  load_config_or_die(config,
                     "setoptions rack MUZAKY pindex 0 score 58 exch 0 eq 0");
  status = infer_for_test(config, game, inference);
  assert(status == INFERENCE_STATUS_SUCCESS);
  // Letters not possible:
  // A - YAKUZA
  // B - ZAMBUK
  // K - none in bag
  // Q - QUAKY
  // Z - none in bag
  assert(get_weight(inference->leave_record->equity_values) == 83);
  assert(get_cardinality(inference->leave_record->equity_values) == 22);
  for (uint32_t i = 0; i < ld->size; i++) {
    if (i == human_readable_letter_to_machine_letter(ld, "A") ||
        i == human_readable_letter_to_machine_letter(ld, "B") ||
        i == human_readable_letter_to_machine_letter(ld, "K") ||
        i == human_readable_letter_to_machine_letter(ld, "Q") ||
        i == human_readable_letter_to_machine_letter(ld, "Z")) {
      assert(get_subtotal(inference->leave_record, i, 1,
                          INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 0);
      assert(get_subtotal(inference->leave_record, i, 1,
                          INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE) == 0);
    } else {
      assert(get_subtotal(inference->leave_record, i, 1,
                          INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) != 0);
    }
  }
  get_stat_for_letter(inference->leave_record, letter_stat,
                      human_readable_letter_to_machine_letter(ld, "E"));
  assert(within_epsilon(get_mean(letter_stat), (double)12 / 83));
  assert(within_epsilon(get_probability_for_random_minimum_draw(
                            inference->bag_as_rack, inference->leave,
                            human_readable_letter_to_machine_letter(ld, "Q"), 1,
                            6),
                        (double)1 / 94));
  assert(within_epsilon(get_probability_for_random_minimum_draw(
                            inference->bag_as_rack, inference->leave,
                            human_readable_letter_to_machine_letter(ld, "B"), 1,
                            6),
                        (double)2 / 94));
  // Both game racks should be empty
  assert(game->players[0]->rack->number_of_letters == 0);
  assert(game->players[1]->rack->number_of_letters == 0);

  load_config_or_die(config,
                     "setoptions rack MUZAK pindex 0 score 50 exch 0 eq 0");
  status = infer_for_test(config, game, inference);
  assert(status == INFERENCE_STATUS_SUCCESS);
  // Can't have B or Y because of ZAMBUK and MUZAKY
  // Can't have K or Z because there are none in the bag
  for (uint32_t i = 0; i < ld->size; i++) {
    if (i == human_readable_letter_to_machine_letter(ld, "B") ||
        i == human_readable_letter_to_machine_letter(ld, "K") ||
        i == human_readable_letter_to_machine_letter(ld, "Y") ||
        i == human_readable_letter_to_machine_letter(ld, "Z")) {
      assert(get_subtotal(inference->leave_record, i, 1,
                          INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 0);
    } else {
      assert(get_subtotal(inference->leave_record, i, 1,
                          INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) != 0);
    }
  }
  assert(within_epsilon(get_probability_for_random_minimum_draw(
                            inference->bag_as_rack, inference->leave,
                            human_readable_letter_to_machine_letter(ld, "B"), 2,
                            5),
                        (double)1 / choose(95, 2)));
  // Both game racks should be empty
  assert(game->players[0]->rack->number_of_letters == 0);
  assert(game->players[1]->rack->number_of_letters == 0);

  load_cgp(game, VS_JEREMY_WITH_P2_RACK);
  // Score doesn't matter since the bag
  // is empty and the inference should just be
  // the remaining tiles exactly. Since the played
  // tiles contain an E, the inferred leave should not
  // contain an E.
  load_config_or_die(config, "setoptions rack E pindex 0 score 22 exch 0 eq 0");
  status = infer_for_test(config, game, inference);
  assert(status == INFERENCE_STATUS_SUCCESS);
  assert(get_weight(inference->leave_record->equity_values) == 1);
  assert(get_cardinality(inference->leave_record->equity_values) == 1);
  set_rack_to_string(ld, rack, "DDSW??");
  assert(within_epsilon(get_mean(inference->leave_record->equity_values),
                        get_leave_value(klv, rack)));
  for (uint32_t i = 0; i < ld->size; i++) {
    if (i == human_readable_letter_to_machine_letter(ld, "S") ||
        i == human_readable_letter_to_machine_letter(ld, "W")) {
      assert(get_subtotal(inference->leave_record, i, 1,
                          INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 1);
      assert(get_subtotal(inference->leave_record, i, 1,
                          INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE) == 1);
    } else if (i == human_readable_letter_to_machine_letter(ld, "D") ||
               i == human_readable_letter_to_machine_letter(ld, "?")) {
      assert(get_subtotal(inference->leave_record, i, 2,
                          INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 1);
      assert(get_subtotal(inference->leave_record, i, 2,
                          INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE) == 1);
    } else {
      assert(get_subtotal(inference->leave_record, i, 1,
                          INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 0);
    }
  }
  reset_game(game);

  load_config_or_die(config,
                     "setoptions rack RENT pindex 0 score 8 exch 0 eq 0");
  status = infer_for_test(config, game, inference);
  assert(status == INFERENCE_STATUS_SUCCESS);
  // There are only 3 racks for which playing RENT for 8 on the opening is
  // top
  // equity: 1) ?ENNRRT keeping ?NR = 2 * 5 * 5  = 50 possible draws 2)
  // EENRRTT
  // keeping ERT = 11 * 5 * 5 = 275 possible draws 3) ENNRRTT keeping NRT =
  // 5 *
  // 5 * 5  = 125 possible draws which sums to 450 total draws. We use this
  // case
  // to easily check that the combinatorial math is correct
  assert(get_weight(inference->leave_record->equity_values) == 450);
  assert(get_cardinality(inference->leave_record->equity_values) == 3);
  for (uint32_t i = 0; i < ld->size; i++) {
    if (i == human_readable_letter_to_machine_letter(ld, "?")) {
      // The blank was only in leave 1
      assert(get_subtotal(inference->leave_record, i, 1,
                          INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 50);
      assert(get_subtotal(inference->leave_record, i, 1,
                          INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE) == 1);
    } else if (i == human_readable_letter_to_machine_letter(ld, "E")) {
      // The E was only in leave 2
      assert(get_subtotal(inference->leave_record, i, 1,
                          INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 275);
      assert(get_subtotal(inference->leave_record, i, 1,
                          INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE) == 1);
    } else if (i == human_readable_letter_to_machine_letter(ld, "N")) {
      // The N was in leaves 1 and 3
      assert(get_subtotal(inference->leave_record, i, 1,
                          INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 175);
      assert(get_subtotal(inference->leave_record, i, 1,
                          INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE) == 2);
    } else if (i == human_readable_letter_to_machine_letter(ld, "R")) {
      // The R was found in all of the leaves
      assert(get_subtotal(inference->leave_record, i, 1,
                          INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 450);
      assert(get_subtotal(inference->leave_record, i, 1,
                          INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE) == 3);
    } else if (i == human_readable_letter_to_machine_letter(ld, "T")) {
      assert(get_subtotal(inference->leave_record, i, 1,
                          INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 400);
    } else {
      assert(get_subtotal(inference->leave_record, i, 1,
                          INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 0);
      assert(get_subtotal(inference->leave_record, i, 1,
                          INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE) == 0);
    }
  }
  get_stat_for_letter(inference->leave_record, letter_stat,
                      human_readable_letter_to_machine_letter(ld, "E"));
  assert(within_epsilon(get_mean(letter_stat), (double)275 / 450));
  assert(within_epsilon(get_stdev(letter_stat), 0.48749802152178456360));
  get_stat_for_letter(inference->leave_record, letter_stat,
                      human_readable_letter_to_machine_letter(ld, "R"));
  assert(within_epsilon(get_mean(letter_stat), 1));
  assert(within_epsilon(get_stdev(letter_stat), 0));

  assert(within_epsilon(get_stdev(inference->leave_record->equity_values),
                        6.53225818584641171327));

  // Contrive an impossible situation to easily test
  // more combinatorics
  load_cgp(game, OOPSYCHOLOGY_CGP);

  // Empty the bag
  while (!bag_is_empty(bag)) {
    draw_random_letter(bag, 0);
  }

  add_letter(bag, human_readable_letter_to_machine_letter(ld, "I"), 0);
  add_letter(bag, human_readable_letter_to_machine_letter(ld, "I"), 0);
  add_letter(bag, human_readable_letter_to_machine_letter(ld, "I"), 0);
  add_letter(bag, human_readable_letter_to_machine_letter(ld, "I"), 0);
  add_letter(bag, human_readable_letter_to_machine_letter(ld, "I"), 0);
  add_letter(bag, human_readable_letter_to_machine_letter(ld, "I"), 0);
  add_letter(bag, human_readable_letter_to_machine_letter(ld, "I"), 0);
  add_letter(bag, human_readable_letter_to_machine_letter(ld, "E"), 0);
  add_letter(bag, human_readable_letter_to_machine_letter(ld, "E"), 0);
  add_letter(bag, human_readable_letter_to_machine_letter(ld, "E"), 0);
  add_letter(bag, human_readable_letter_to_machine_letter(ld, "E"), 0);
  add_letter(bag, human_readable_letter_to_machine_letter(ld, "Z"), 0);

  // Z(OOPSYCHOLOGY) is over 100 points so keeping the Z will never be
  // inferred
  // for plays scoring 50.
  load_config_or_die(config,
                     "setoptions rack IIII pindex 0 score 50 exch 0 eq 0");
  status = infer_for_test(config, game, inference);
  assert(status == INFERENCE_STATUS_SUCCESS);
  // There are only 4 racks for which not playing Z(OOPSYCHOLOGY) is
  // correct:
  // EEEIIII = 4 possible draws for E = 4 total
  // EEIIIII = 6 possible draws for the Es * 3 possible draws for the Is =
  // 18
  // total EIIIIII = 4 possible draws for E = 4  * 3 possible draws for the
  // Is =
  // 12 IIIIIII = 1 possible draw for the Is = 1 For a total of 35
  // possible draws
  assert(get_weight(inference->leave_record->equity_values) == 35);
  assert(get_cardinality(inference->leave_record->equity_values) == 4);
  for (uint32_t i = 0; i < ld->size; i++) {
    if (i == human_readable_letter_to_machine_letter(ld, "E")) {
      assert(get_subtotal(inference->leave_record, i, 1,
                          INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 12);
      assert(get_subtotal_sum_with_minimum(
                 inference->leave_record, i, 1,
                 INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 34);
      assert(get_subtotal(inference->leave_record, i, 1,
                          INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE) == 1);
      assert(get_subtotal_sum_with_minimum(
                 inference->leave_record, i, 1,
                 INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE) == 3);

      assert(get_subtotal(inference->leave_record, i, 2,
                          INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 18);
      assert(get_subtotal_sum_with_minimum(
                 inference->leave_record, i, 2,
                 INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 22);
      assert(get_subtotal(inference->leave_record, i, 2,
                          INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE) == 1);
      assert(get_subtotal_sum_with_minimum(
                 inference->leave_record, i, 2,
                 INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE) == 2);

      assert(get_subtotal(inference->leave_record, i, 3,
                          INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 4);
      assert(get_subtotal_sum_with_minimum(
                 inference->leave_record, i, 3,
                 INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 4);
      assert(get_subtotal(inference->leave_record, i, 3,
                          INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE) == 1);
      assert(get_subtotal_sum_with_minimum(
                 inference->leave_record, i, 3,
                 INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE) == 1);
    } else if (i == human_readable_letter_to_machine_letter(ld, "I")) {
      assert(get_subtotal(inference->leave_record, i, 1,
                          INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 18);
      assert(get_subtotal_sum_with_minimum(
                 inference->leave_record, i, 1,
                 INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 31);
      assert(get_subtotal(inference->leave_record, i, 1,
                          INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE) == 1);
      assert(get_subtotal_sum_with_minimum(
                 inference->leave_record, i, 1,
                 INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE) == 3);

      assert(get_subtotal(inference->leave_record, i, 2,
                          INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 12);
      assert(get_subtotal_sum_with_minimum(
                 inference->leave_record, i, 2,
                 INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 13);
      assert(get_subtotal(inference->leave_record, i, 2,
                          INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE) == 1);
      assert(get_subtotal_sum_with_minimum(
                 inference->leave_record, i, 2,
                 INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE) == 2);

      assert(get_subtotal(inference->leave_record, i, 3,
                          INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 1);
      assert(get_subtotal_sum_with_minimum(
                 inference->leave_record, i, 3,
                 INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 1);
      assert(get_subtotal(inference->leave_record, i, 3,
                          INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE) == 1);
      assert(get_subtotal_sum_with_minimum(
                 inference->leave_record, i, 3,
                 INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE) == 1);
    } else {
      assert(get_subtotal(inference->leave_record, i, 1,
                          INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 0);
      assert(get_subtotal_sum_with_minimum(
                 inference->leave_record, i, 1,
                 INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 0);
      assert(get_subtotal(inference->leave_record, i, 1,
                          INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE) == 0);
      assert(get_subtotal_sum_with_minimum(
                 inference->leave_record, i, 1,
                 INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE) == 0);
    }
  }
  assert(within_epsilon(get_probability_for_random_minimum_draw(
                            inference->bag_as_rack, inference->leave,
                            human_readable_letter_to_machine_letter(ld, "E"), 3,
                            4),
                        (double)4 / choose(8, 3)));
  reset_game(game);

  // Check that the equity margin works
  load_config_or_die(config,
                     "setoptions rack MUZAKY pindex 0 score 58 exch 0 eq 5");
  status = infer_for_test(config, game, inference);
  assert(status == INFERENCE_STATUS_SUCCESS);
  // Letters not possible with equity margin of 5:
  // B - ZAMBUK
  // K - none in bag
  // Q - QUAKY
  // Z - none in bag
  // Letters now possible because of the additional 5 equity buffer:
  // A - YAKUZA
  // 2 Bs and 1 Q with 6 played tiles is 100 - (2 + 1 + 6) = 91
  assert(get_weight(inference->leave_record->equity_values) == 91);
  // All letters except the 4 described above are possible, so 27 - 4 = 23
  assert(get_cardinality(inference->leave_record->equity_values) == 23);
  for (uint32_t i = 0; i < ld->size; i++) {
    if (i == human_readable_letter_to_machine_letter(ld, "B") ||
        i == human_readable_letter_to_machine_letter(ld, "K") ||
        i == human_readable_letter_to_machine_letter(ld, "Q") ||
        i == human_readable_letter_to_machine_letter(ld, "Z")) {
      assert(get_subtotal(inference->leave_record, i, 1,
                          INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 0);
    } else {
      assert(get_subtotal(inference->leave_record, i, 1,
                          INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) != 0);
    }
  }

  // Test partial leaves
  // play GRIND with partial leave of ?
  // Partially known leaves that are on the player's rack
  // before the inference are not removed from the bag, so
  // we have to remove it here.
  set_rack_to_string(ld, game->players[0]->rack, "?");
  draw_letter(bag, human_readable_letter_to_machine_letter(ld, "?"), 0);
  load_config_or_die(config,
                     "setoptions rack GRIND pindex 0 score 18 exch 0 eq 0");
  status = infer_for_test(config, game, inference);
  assert(status == INFERENCE_STATUS_SUCCESS);
  // If GRIND is played keeping ?, the only
  // possible other tile is an X
  assert(get_weight(inference->leave_record->equity_values) == 2);
  assert(get_cardinality(inference->leave_record->equity_values) == 1);
  set_rack_to_string(ld, rack, "X?");
  assert(within_epsilon(get_mean(inference->leave_record->equity_values),
                        get_leave_value(klv, rack)));
  for (uint32_t i = 0; i < ld->size; i++) {
    if (i == human_readable_letter_to_machine_letter(ld, "?") ||
        i == human_readable_letter_to_machine_letter(ld, "X")) {
      assert(get_subtotal(inference->leave_record, i, 1,
                          INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 2);
      assert(get_subtotal(inference->leave_record, i, 1,
                          INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE) == 1);
    } else {
      assert(get_subtotal(inference->leave_record, i, 1,
                          INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 0);
    }
  }
  reset_game(game);

  set_rack_to_string(ld, game->players[0]->rack, "H");
  draw_letter(bag, human_readable_letter_to_machine_letter(ld, "H"), 0);
  load_config_or_die(config,
                     "setoptions rack RIN pindex 0 score 6 exch 0 eq 0");
  status = infer_for_test(config, game, inference);
  // If the player opens with RIN for 6 keeping an H, there are only 3
  // possible racks where this would be correct:
  // 1) ?HIINRR keeping ?HIR = 2 * 2 * 8 * 5 = 160
  // 2) ?HINNRR keeping ?HNR = 2 * 2 * 5 * 5 = 100
  // 3) HIINNRR keeping HINR = 2 * 8 * 5 * 5 = 400
  // For a total of 660 possible draws
  assert(status == INFERENCE_STATUS_SUCCESS);
  assert(get_weight(inference->leave_record->equity_values) == 660);
  assert(get_cardinality(inference->leave_record->equity_values) == 3);
  set_rack_to_string(ld, rack, "?HIR");
  double bhir_value = get_leave_value(klv, rack);
  double bhir_weighted_value = bhir_value * 160;
  set_rack_to_string(ld, rack, "?HNR");
  double bhnr_value = get_leave_value(klv, rack);
  double bhnr_weighted_value = bhnr_value * 100;
  set_rack_to_string(ld, rack, "HINR");
  double hirn_value = get_leave_value(klv, rack);
  double hirn_weighted_value = hirn_value * 400;
  double mean_rin_leave_value =
      (bhir_weighted_value + bhnr_weighted_value + hirn_weighted_value) / 660;
  assert(within_epsilon(get_mean(inference->leave_record->equity_values),
                        mean_rin_leave_value));

  // Test exchanges
  load_cgp(game, VS_JEREMY);
  // Take out good letters and throw in bad ones to force certain
  // racks to have exchange as the best play
  draw_letter(bag, human_readable_letter_to_machine_letter(ld, "?"), 0);
  draw_letter(bag, human_readable_letter_to_machine_letter(ld, "?"), 0);
  draw_letter(bag, human_readable_letter_to_machine_letter(ld, "E"), 0);
  draw_letter(bag, human_readable_letter_to_machine_letter(ld, "A"), 0);

  add_letter(bag, human_readable_letter_to_machine_letter(ld, "Q"), 0);
  add_letter(bag, human_readable_letter_to_machine_letter(ld, "W"), 0);
  add_letter(bag, human_readable_letter_to_machine_letter(ld, "W"), 0);
  add_letter(bag, human_readable_letter_to_machine_letter(ld, "V"), 0);
  add_letter(bag, human_readable_letter_to_machine_letter(ld, "V"), 0);

  reset_rack(rack);
  load_config_or_die(config, "setoptions rack " EMPTY_RACK_STRING
                             " pindex 0 score 0 exch 6 eq 0");
  status = infer_for_test(config, game, inference);

  assert(status == INFERENCE_STATUS_SUCCESS);
  // Keeping any one of D, H, R, or S is valid
  assert(get_subtotal(inference->leave_record,
                      human_readable_letter_to_machine_letter(ld, "D"), 1,
                      INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) != 0);
  assert(get_subtotal(inference->leave_record,
                      human_readable_letter_to_machine_letter(ld, "H"), 1,
                      INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) != 0);
  assert(get_subtotal(inference->leave_record,
                      human_readable_letter_to_machine_letter(ld, "R"), 1,
                      INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) != 0);
  assert(get_subtotal(inference->leave_record,
                      human_readable_letter_to_machine_letter(ld, "S"), 1,
                      INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) != 0);

  // There are exchanges where throwing back at least one
  // of these is correct
  assert(get_subtotal(inference->exchanged_record,
                      human_readable_letter_to_machine_letter(ld, "D"), 1,
                      INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) != 0);
  assert(get_subtotal(inference->exchanged_record,
                      human_readable_letter_to_machine_letter(ld, "L"), 1,
                      INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) != 0);
  assert(get_subtotal(inference->exchanged_record,
                      human_readable_letter_to_machine_letter(ld, "Q"), 1,
                      INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) != 0);
  assert(get_subtotal(inference->exchanged_record,
                      human_readable_letter_to_machine_letter(ld, "V"), 1,
                      INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) != 0);
  assert(get_subtotal(inference->exchanged_record,
                      human_readable_letter_to_machine_letter(ld, "W"), 1,
                      INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) != 0);

  // Exchanges with the I are never correct
  assert(get_subtotal(inference->leave_record,
                      human_readable_letter_to_machine_letter(ld, "I"), 1,
                      INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 0);
  assert(get_subtotal(inference->exchanged_record,
                      human_readable_letter_to_machine_letter(ld, "I"), 1,
                      INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW) == 0);

  reset_game(game);

  destroy_stat(letter_stat);
  destroy_rack(rack);
  destroy_inference(inference);
  destroy_game(game);
}

void test_infer(TestConfig *testconfig) {
  test_trivial_random_probability(testconfig);
  test_infer_rack_overflow(testconfig);
  test_infer_no_tiles_played_rack_empty(testconfig);
  test_infer_no_tiles_played_rack_null(testconfig);
  test_infer_both_play_and_exchange(testconfig);
  test_infer_exchange_score_not_zero(testconfig);
  test_infer_exchange_not_allowed(testconfig);
  test_infer_tiles_played_not_in_bag(testconfig);
  test_infer_nonerror_cases(testconfig, 1);
  test_infer_nonerror_cases(testconfig, 2);
  test_infer_nonerror_cases(testconfig, 7);
}
