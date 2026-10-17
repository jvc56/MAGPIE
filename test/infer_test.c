#include "infer_test.h"

#include "../src/def/inference_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/alias_method.h"
#include "../src/ent/bag.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/game_history.h"
#include "../src/ent/inference_args.h"
#include "../src/ent/inference_results.h"
#include "../src/ent/klv.h"
#include "../src/ent/leave_rack.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/stats.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/xoshiro.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/inference.h"
#include "../src/util/io_util.h"
#include "../src/util/math_util.h"
#include "../src/util/string_util.h"
#include "test_constants.h"
#include "test_util.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

error_code_t infer_for_test(InferenceCtx **ctx, const Config *config,
                            int target_index, int target_score,
                            int target_num_exch,
                            const char *target_played_tiles_str,
                            const char *target_known_rack_str,
                            const char *nontarget_known_rack_str,
                            InferenceResults *inference_results) {
  const Game *game = config_get_game(config);
  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);

  Rack target_played_tiles;
  rack_set_dist_size_and_reset(&target_played_tiles, ld_size);
  if (target_played_tiles_str != NULL) {
    rack_set_to_string(ld, &target_played_tiles, target_played_tiles_str);
  }

  Rack target_known_rack;
  rack_set_dist_size_and_reset(&target_known_rack, ld_size);
  if (target_known_rack_str != NULL) {
    rack_set_to_string(ld, &target_known_rack, target_known_rack_str);
  }

  Rack nontarget_known_rack;
  rack_set_dist_size_and_reset(&nontarget_known_rack, ld_size);
  if (nontarget_known_rack_str != NULL) {
    rack_set_to_string(ld, &nontarget_known_rack, nontarget_known_rack_str);
  }

  ErrorStack *error_stack = error_stack_create();
  thread_control_set_status(config_get_thread_control(config),
                            THREAD_CONTROL_STATUS_STARTED);

  InferenceArgs args;
  infer_args_fill(
      &args, config_get_num_plays(config),
      config_get_eq_margin_inference(config), NULL, game,
      config_get_num_threads(config), 0, config_get_print_interval(config),
      config_get_thread_control(config), false, true, target_index,
      int_to_equity(target_score), target_num_exch, &target_played_tiles,
      &target_known_rack, &nontarget_known_rack);
  if (!ctx) {
    infer_without_ctx(&args, inference_results, error_stack);
  } else {
    infer(&args, ctx, inference_results, error_stack);
  }
  error_code_t status = error_stack_top(error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
  }
  error_stack_destroy(error_stack);
  return status;
}

// Assumes the config game history has been loaded
error_code_t
infer_for_test_with_history(Config *config, InferenceResults *inference_results,
                            const int num_events_to_play,
                            const char *play_on_turn_known_rack_str) {
  Game *game = config_get_game(config);
  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);
  Rack target_played_tiles;
  rack_set_dist_size_and_reset(&target_played_tiles, ld_size);
  Rack target_known_rack;
  rack_set_dist_size_and_reset(&target_known_rack, ld_size);
  Rack nontarget_known_rack;
  rack_set_dist_size_and_reset(&nontarget_known_rack, ld_size);
  ErrorStack *error_stack = error_stack_create();
  thread_control_set_status(config_get_thread_control(config),
                            THREAD_CONTROL_STATUS_STARTED);
  GameHistory *game_history = config_get_game_history(config);
  if (game_history_get_num_events(game_history) != 0) {
    game_history_goto(game_history, num_events_to_play, error_stack);
    if (error_stack_is_empty(error_stack)) {
      game_play_n_events(game_history, game,
                         game_history_get_num_played_events(game_history),
                         false, error_stack);
    }
    if (!error_stack_is_empty(error_stack)) {
      error_stack_print_and_reset(error_stack);
      assert(0);
    }
  }
  if (!is_string_empty_or_null(play_on_turn_known_rack_str)) {
    StringBuilder *set_rack_cmd_sb = string_builder_create();
    string_builder_add_formatted_string(set_rack_cmd_sb, "r %s",
                                        play_on_turn_known_rack_str);
    load_and_exec_config_or_die(config, string_builder_peek(set_rack_cmd_sb));
    string_builder_destroy(set_rack_cmd_sb);
  }
  config_infer(config, true, 0, 0, 0, &target_played_tiles, &target_known_rack,
               &nontarget_known_rack, true, inference_results, error_stack);
  error_code_t status = error_stack_top(error_stack);
  error_stack_destroy(error_stack);
  return status;
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

  error_code_t status = infer_for_test(NULL, config, 0, 0, 0, "ABCDEFGH", "",
                                       "", inference_results);
  assert(status == ERROR_STATUS_INFERENCE_RACK_OVERFLOW);
  game_reset(game);

  status = infer_for_test(NULL, config, 0, 0, 0, "DEFGH", "ABC", "",
                          inference_results);
  assert(status == ERROR_STATUS_INFERENCE_RACK_OVERFLOW);

  inference_results_destroy(inference_results);
  config_destroy(config);
}

// A target move with zero played and zero exchanged tiles is a pass: there
// is no evidence to infer a leave from. This isn't an error -- callers like
// analyze or annotation mode replay whole games including passes, and
// shouldn't have to special-case them -- so infer returns an empty, valid
// result instead.
void test_infer_no_tiles_played_rack_empty(void) {
  Config *config =
      config_create_or_die("set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 "
                           "all -r2 all -numplays 1");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);

  ErrorStack *error_stack = error_stack_create();
  InferenceResults *inference_results = inference_results_create(NULL);
  error_code_t status =
      infer_for_test(NULL, config, 0, 0, 0, "", "", "", inference_results);
  assert(status == ERROR_STATUS_SUCCESS);
  assert(inference_results_get_valid_for_current_game_state(inference_results));
  assert(leave_rack_list_get_count(
             inference_results_get_leave_rack_list(inference_results)) == 0);

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
      infer_for_test(NULL, config, 0, 0, 1, "DEFGH", "", "", inference_results);
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
      infer_for_test(NULL, config, 0, 3, 1, "", "", "", inference_results);
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
      infer_for_test(NULL, config, 0, 3, 1, "", "", "", inference_results);
  assert(status == ERROR_STATUS_INFERENCE_EXCHANGE_NOT_ALLOWED);

  bag_add_letter(bag, BLANK_MACHINE_LETTER, 0);
  // There should now be 14 tiles in the bag
  status = infer_for_test(NULL, config, 0, 3, 1, "", "", "", inference_results);
  assert(status == ERROR_STATUS_INFERENCE_EXCHANGE_SCORE_NOT_ZERO);

  const char *cgp_16_unseen =
      "2CHIGOE3T3/3O6NAE2/3M6END2/2AY6UG1G1/2B7MO1L1/1VODKA5i1OD/2R2TANNERS1RE/"
      "OUTA2ZA3T1IF/2I1CRoWDIE2A1/2V6FEEBS1/1NEWISHLY1LEA2/15/15/15/15 "
      "AEEIJNX/ILOOPQR 437/289 0";
  load_cgp_or_die(game, cgp_16_unseen);
  load_and_exec_config_or_die(config, "s");
  // The bag has 16, exchanges should be allowed
  status = infer_for_test(NULL, config, 1, 0, 5, "", "", "AEEIJNX",
                          inference_results);
  assert(status == ERROR_STATUS_SUCCESS);

  const char *cgp_14_unseen =
      "2CHIGOE3T3/3O6NAE2/3M6END2/2AY6UG1G1/2B7MO1L1/1VODKA5i1OD/2R2TANNERS1RE/"
      "OUTA2ZA3T1IF/2I1CRoWDIE2A1/2V6FEEBS1/1NEWISHLY1LEA2/JA13/15/15/15 "
      "ILOOPQR/EEINTTX 289/456 0";
  load_cgp_or_die(game, cgp_14_unseen);
  load_and_exec_config_or_die(config, "s");
  // The bag has 14, exchanges should be allowed
  status = infer_for_test(NULL, config, 1, 0, 5, "", "", "ILOOPQR",
                          inference_results);
  assert(status == ERROR_STATUS_SUCCESS);

  const char *cgp_13_unseen =
      "2CHIGOE3T3/3O6NAE2/3M6END2/1PAY6UG1G1/2B7MO1L1/1VODKA5i1OD/"
      "2R2TANNERS1RE/OUTA2ZA3T1IF/2I1CRoWDIE2A1/2V6FEEBS1/1NEWISHLY1LEA2/JA13/"
      "15/15/15 EEINTTX/ILOOQRU 456/297 0";
  load_cgp_or_die(game, cgp_13_unseen);
  load_and_exec_config_or_die(config, "s");
  // The bag has 13, exchanges should not be allowed
  status = infer_for_test(NULL, config, 1, 0, 5, "", "", "EEINTTX",
                          inference_results);
  assert(status == ERROR_STATUS_INFERENCE_EXCHANGE_NOT_ALLOWED);

  inference_results_destroy(inference_results);
  config_destroy(config);
}

void test_infer_tiles_played_not_in_bag(void) {
  Config *config =
      config_create_or_die("set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 "
                           "all -r2 all -numplays 1");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);

  InferenceResults *inference_results = inference_results_create(NULL);
  load_and_exec_config_or_die(config, "set -ima 0 -threads 1");
  error_code_t status = infer_for_test(NULL, config, 0, 0, 1, "ACBYEYY", "", "",
                                       inference_results);
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
  load_and_exec_config_or_die(config, "set -ima 0 -threads 1");
  error_code_t status =
      infer_for_test_with_history(config, inference_results, 0, "");
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

  InferenceCtx *ctx = NULL;

  load_and_exec_config_or_die(config, "set -numplays 20");
  if (use_game_history) {
    load_game_history_with_gcg_string(config, gcg_string_header,
                                      ">Tim: MUZAKS 8H MUZAKS +52 52");
    status = infer_for_test_with_history(config, inference_results, 1, "");
  } else {
    status = infer_for_test(&ctx, config, 0, 52, 0, "MUZAKS", "", "",
                            inference_results);
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
    status = infer_for_test_with_history(config, inference_results, 1, "");
  } else {
    status = infer_for_test(&ctx, config, 0, 58, 0, "MUZAKY", "", "",
                            inference_results);
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
    status = infer_for_test_with_history(config, inference_results, 1, "");
  } else {
    status = infer_for_test(&ctx, config, 0, 50, 0, "MUZAK", "", "",
                            inference_results);
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

  // Check that inference works with nontarget known rack
  if (use_game_history) {
    load_game_history_with_gcg_string(config, gcg_string_header,
                                      ">Tim: MUZAK 8H MUZAK +50 50");
    status = infer_for_test_with_history(config, inference_results, 1, "SSSS");
  } else {
    load_and_exec_config_or_die(config, "r SSSS");
    status = infer_for_test(&ctx, config, 0, 50, 0, "MUZAK", "", "SSSS",
                            inference_results);
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
        i == ld_hl_to_ml(ld, "Y") || i == ld_hl_to_ml(ld, "Z") ||
        i == ld_hl_to_ml(ld, "S")) {
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
      (double)1 / choose(91, 2)));

  load_cgp_or_die(game, VS_JEREMY_WITH_P2_RACK);
  // Score doesn't matter since the bag
  // is empty and the inference_results should just be
  // the remaining tiles exactly. Since the played
  // tiles contain an E, the inferred leave should not
  // contain an E.
  load_and_exec_config_or_die(config, "set -ima 10000");
  if (use_game_history) {
    load_game_history_with_gcg(config, "vs_jeremy");
    status = infer_for_test_with_history(
        config, inference_results,
        game_history_get_num_events(config_get_game_history(config)), "");
  } else {
    status = infer_for_test(&ctx, config, 0, 32, 0, "DEW??", "", "AHIILR",
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
    load_and_exec_config_or_die(config, "set -ima 0");
    load_game_history_with_gcg(config, "vs_jeremy");
    status = infer_for_test_with_history(config, inference_results, 11, "");
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

  load_and_exec_config_or_die(config, "set -numplays 100 -ima 0");
  if (use_game_history) {
    load_game_history_with_gcg_string(config, gcg_string_header,
                                      ">Tim: ERNT 8G RENT +8 8");
    status = infer_for_test_with_history(config, inference_results, 1, "");
  } else {
    status = infer_for_test(&ctx, config, 0, 8, 0, "ENRT", "", "",
                            inference_results);
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
  load_and_exec_config_or_die(config, "set -ima 0");
  status =
      infer_for_test(&ctx, config, 0, 50, 0, "IIII", "", "", inference_results);
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
  load_and_exec_config_or_die(config, "set -ima 5");
  if (use_game_history) {
    load_game_history_with_gcg_string(config, gcg_string_header,
                                      ">Tim: MUZAKY 8H MUZAKY +58 58");
    status = infer_for_test_with_history(config, inference_results, 1, "");
  } else {
    status = infer_for_test(&ctx, config, 0, 58, 0, "MUZAKY", "", "",
                            inference_results);
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

  load_and_exec_config_or_die(config, "set -ima 0");
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
    status = infer_for_test_with_history(config, inference_results, 4, "");
  } else {
    status = infer_for_test(&ctx, config, 0, 18, 0, "GRIND", "?", "",
                            inference_results);
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

  load_and_exec_config_or_die(config, "set -ima 0");
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
    status = infer_for_test_with_history(config, inference_results, 4, "");
  } else {
    status = infer_for_test(&ctx, config, 0, 6, 0, "RIN", "H", "",
                            inference_results);
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
  load_and_exec_config_or_die(config, "set -ima 0");
  if (use_game_history) {
    GameHistory *game_history = config_get_game_history(config);
    test_parse_gcg("exchange_with_seven_in_bag", config, game_history);
    status = infer_for_test_with_history(
        config, inference_results,
        game_history_get_num_events(config_get_game_history(config)), "");
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

    status =
        infer_for_test(&ctx, config, 0, 0, 6, "", "", "", inference_results);

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

  inference_ctx_destroy(ctx);

  stat_destroy(letter_stat);
  rack_destroy(rack);
  inference_results_destroy(inference_results);
  prng_destroy(prng);
  config_destroy(config);
}

// Infers an exchange of num_exch tiles by the player on turn, who is the
// target, from a rack the observer knows in full, so that exactly one rack is
// evaluated. Returns whether the inference accepted that rack.
static bool infer_known_rack_exchange(const Config *config,
                                      const char *rack_str, int num_exch,
                                      bool use_cutoff,
                                      InferenceResults *inference_results) {
  const LetterDistribution *ld = config_get_ld(config);
  const int ld_size = ld_get_size(ld);
  Rack target_played_tiles;
  rack_set_dist_size_and_reset(&target_played_tiles, ld_size);
  Rack target_known_rack;
  rack_set_dist_size_and_reset(&target_known_rack, ld_size);
  rack_set_to_string(ld, &target_known_rack, rack_str);
  Rack nontarget_known_rack;
  rack_set_dist_size_and_reset(&nontarget_known_rack, ld_size);
  ErrorStack *error_stack = error_stack_create();
  thread_control_set_status(config_get_thread_control(config),
                            THREAD_CONTROL_STATUS_STARTED);
  config_infer(
      config, false, game_get_player_on_turn_index(config_get_game(config)), 0,
      num_exch, &target_played_tiles, &target_known_rack, &nontarget_known_rack,
      use_cutoff, inference_results, error_stack);
  assert(error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);
  return stat_get_num_samples(inference_results_get_equity_values(
             inference_results, INFERENCE_TYPE_RACK)) > 0;
}

static void assert_known_rack_exchange_accepted(const Config *config,
                                                const char *rack_str,
                                                int num_exch, bool expected) {
  InferenceResults *inference_results = inference_results_create(NULL);
  for (int cutoff_idx = 0; cutoff_idx < 2; cutoff_idx++) {
    const bool use_cutoff = cutoff_idx == 1;
    const bool accepted = infer_known_rack_exchange(
        config, rack_str, num_exch, use_cutoff, inference_results);
    if (accepted != expected) {
      printf("exchange of %d from %s: expected %s, got %s (cutoff %s)\n",
             num_exch, rack_str, expected ? "accepted" : "rejected",
             accepted ? "accepted" : "rejected", use_cutoff ? "on" : "off");
      assert(0);
    }
  }
  inference_results_destroy(inference_results);
}

// An exchange of N tiles is consistent with a rack when no move beats the
// rack's best exchange of N tiles by more than the inference margin, the
// same test a tile placement's equity gets. The equities below are from the
// test CSW21 leaves on an empty board, with the best exchange of 5 keeping
// the blanks (37.559 points).
void test_infer_exchange_margin(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1 -threads 1 -ima 5");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);

  // The exchange is itself the best move.
  assert_known_rack_exchange_accepted(config, "BBJVW??", 5, true);
  // A play (41.429) beats the exchange by 3.870, inside the margin.
  assert_known_rack_exchange_accepted(config, "BFNTT??", 5, true);
  // A play (43.753) beats the exchange by 6.194, outside the margin.
  assert_known_rack_exchange_accepted(config, "GNRRR??", 5, false);
  // An exchange of another size (37.753) and a play (39.397) both beat the
  // exchange of 5, the play by 1.838: exchanges of other sizes are ordinary
  // moves, so this is inside the margin.
  assert_known_rack_exchange_accepted(config, "BLNRV??", 5, true);

  load_and_exec_config_or_die(config, "set -ima 3");
  assert_known_rack_exchange_accepted(config, "BFNTT??", 5, false);
  assert_known_rack_exchange_accepted(config, "BLNRV??", 5, true);

  load_and_exec_config_or_die(config, "set -ima 0");
  assert_known_rack_exchange_accepted(config, "BBJVW??", 5, true);
  assert_known_rack_exchange_accepted(config, "BLNRV??", 5, false);

  // An accepted rack records the leave and exchanged tiles of its best
  // exchange of 5, even though a play is its best move.
  load_and_exec_config_or_die(config, "set -ima 5");
  const LetterDistribution *ld = config_get_ld(config);
  InferenceResults *inference_results = inference_results_create(NULL);
  assert(
      infer_known_rack_exchange(config, "BFNTT??", 5, true, inference_results));
  assert(inference_results_get_subtotal(inference_results, INFERENCE_TYPE_LEAVE,
                                        BLANK_MACHINE_LETTER, 2,
                                        INFERENCE_SUBTOTAL_LEAVE) == 1);
  assert(inference_results_get_subtotal(
             inference_results, INFERENCE_TYPE_EXCHANGED, ld_hl_to_ml(ld, "T"),
             2, INFERENCE_SUBTOTAL_LEAVE) == 1);
  assert(inference_results_get_subtotal(
             inference_results, INFERENCE_TYPE_EXCHANGED, ld_hl_to_ml(ld, "B"),
             1, INFERENCE_SUBTOTAL_LEAVE) == 1);
  inference_results_destroy(inference_results);
  config_destroy(config);
}

// Runs the same inference with and without the cutoff optimization, asserts
// that they record the same racks, and returns how many racks were accepted.
static uint64_t
assert_infer_cutoff_matches(const Config *config, bool use_game_history,
                            int num_exch, const char *target_known_rack_str,
                            const char *nontarget_known_rack_str) {
  const LetterDistribution *ld = config_get_ld(config);
  const int ld_size = ld_get_size(ld);
  InferenceResults *results[2] = {inference_results_create(NULL),
                                  inference_results_create(NULL)};
  for (int cutoff_idx = 0; cutoff_idx < 2; cutoff_idx++) {
    Rack target_played_tiles;
    rack_set_dist_size_and_reset(&target_played_tiles, ld_size);
    Rack target_known_rack;
    rack_set_dist_size_and_reset(&target_known_rack, ld_size);
    rack_set_to_string(ld, &target_known_rack, target_known_rack_str);
    Rack nontarget_known_rack;
    rack_set_dist_size_and_reset(&nontarget_known_rack, ld_size);
    rack_set_to_string(ld, &nontarget_known_rack, nontarget_known_rack_str);
    ErrorStack *error_stack = error_stack_create();
    thread_control_set_status(config_get_thread_control(config),
                              THREAD_CONTROL_STATUS_STARTED);
    config_infer(config, use_game_history,
                 game_get_player_on_turn_index(config_get_game(config)), 0,
                 num_exch, &target_played_tiles, &target_known_rack,
                 &nontarget_known_rack, cutoff_idx == 1, results[cutoff_idx],
                 error_stack);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_print_and_reset(error_stack);
      assert(0);
    }
    error_stack_destroy(error_stack);
  }
  assert_inference_results_equal(results[0], results[1], ld_size);
  const uint64_t num_accepted = stat_get_num_unique_samples(
      inference_results_get_equity_values(results[0], INFERENCE_TYPE_RACK));
  inference_results_destroy(results[0]);
  inference_results_destroy(results[1]);
  return num_accepted;
}

// The cutoff optimization only changes how fast inference finds out whether
// a move beats the target's by more than the margin, never the answer: for
// every exchange size, with and without a margin, from explicit arguments
// and from the game history.
void test_infer_exchange_cutoff_matches(void) {
  // The player on turn has ILOOPQR and 14 tiles are unseen to them, the
  // fewest that still allow inferring an exchange.
  const char *cgp_14_unseen =
      "cgp 2CHIGOE3T3/3O6NAE2/3M6END2/2AY6UG1G1/2B7MO1L1/1VODKA5i1OD/"
      "2R2TANNERS1RE/OUTA2ZA3T1IF/2I1CRoWDIE2A1/2V6FEEBS1/1NEWISHLY1LEA2/"
      "JA13/15/15/15 ILOOPQR/EEINTTX 0/0 0 -lex CSW21;";
  const char *margins[] = {"5", "0", NULL};
  uint64_t num_accepted = 0;
  for (int margin_idx = 0; margins[margin_idx]; margin_idx++) {
    char *settings = get_formatted_string(
        "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
        "-numplays 1 -threads 2 -ima %s",
        margins[margin_idx]);
    for (int num_exch = 1; num_exch <= RACK_SIZE; num_exch++) {
      // A new config for each exchange starts a new game history.
      Config *config = config_create_or_die(settings);
      load_and_exec_config_or_die(config, cgp_14_unseen);
      num_accepted +=
          assert_infer_cutoff_matches(config, false, num_exch, "", "EEINTTX");
      // On an empty board, with four of the target's tiles known so that
      // only three are enumerated: a good leave and a bad one.
      load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
      num_accepted +=
          assert_infer_cutoff_matches(config, false, num_exch, "EIRS", "");
      num_accepted +=
          assert_infer_cutoff_matches(config, false, num_exch, "UVVW", "");
      // The same, inferred from the game history.
      load_and_exec_config_or_die(config, "r EUVVWIR");
      char *exchange_cmd =
          get_formatted_string("com ex %.*s", num_exch, "UVVWIRE");
      load_and_exec_config_or_die(config, exchange_cmd);
      free(exchange_cmd);
      num_accepted += assert_infer_cutoff_matches(config, true, 0, "EIRS", "");
      num_accepted += assert_infer_cutoff_matches(config, true, 0, "UVVW", "");
      config_destroy(config);
    }
    // A whole game's history ending in an exchange of 6.
    Config *config = config_create_or_die(settings);
    free(settings);
    load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
    assert(test_parse_gcg("exchange_with_seven_in_bag", config,
                          config_get_game_history(config)) ==
           ERROR_STATUS_SUCCESS);
    game_play_to_end_or_die(config_get_game_history(config),
                            config_get_game(config));
    num_accepted += assert_infer_cutoff_matches(config, true, 0, "", "");
    config_destroy(config);
  }
  // The comparisons above mean something only if racks were accepted.
  assert(num_accepted > 0);
}

// Asserts that two inferences of the same position counted the same leaves.
// The leave list's ties on draws and the low bits of the means may come out
// in a different order with more than one thread, so those are compared by
// draws and within epsilon.
static void
assert_inference_results_count_the_same(InferenceResults *results,
                                        InferenceResults *expected) {
  const int ld_size =
      rack_get_dist_size(inference_results_get_bag_as_rack(expected));
  for (int infer_type_idx = 0; infer_type_idx < NUMBER_OF_INFER_TYPES;
       infer_type_idx++) {
    const Stat *stat = inference_results_get_equity_values(
        results, (inference_stat_t)infer_type_idx);
    const Stat *expected_stat = inference_results_get_equity_values(
        expected, (inference_stat_t)infer_type_idx);
    assert(stat_get_num_samples(stat) == stat_get_num_samples(expected_stat));
    assert(stat_get_num_unique_samples(stat) ==
           stat_get_num_unique_samples(expected_stat));
    assert(within_epsilon(stat_get_mean(stat), stat_get_mean(expected_stat)));
    for (int ml = 0; ml < ld_size; ml++) {
      for (int num_letters = 1; num_letters <= RACK_SIZE; num_letters++) {
        for (int subtotal_idx = 0; subtotal_idx < NUMBER_OF_INFERENCE_SUBTOTALS;
             subtotal_idx++) {
          assert(inference_results_get_subtotal(
                     results, (inference_stat_t)infer_type_idx, ml, num_letters,
                     (inference_subtotal_t)subtotal_idx) ==
                 inference_results_get_subtotal(
                     expected, (inference_stat_t)infer_type_idx, ml,
                     num_letters, (inference_subtotal_t)subtotal_idx));
        }
      }
    }
  }

  // The alias method's items are sorted before its tables are built, so the
  // tables a simulation samples from are identical.
  const AliasMethod *alias_method = inference_results_get_alias_method(results);
  const AliasMethod *expected_alias_method =
      inference_results_get_alias_method(expected);
  assert(alias_method->num_items == expected_alias_method->num_items);
  assert(alias_method->total_item_count ==
         expected_alias_method->total_item_count);
  for (uint32_t item_idx = 0; item_idx < alias_method->num_items; item_idx++) {
    const AliasMethodItem *item = &alias_method->items[item_idx];
    const AliasMethodItem *expected_item =
        &expected_alias_method->items[item_idx];
    assert(memcmp(&item->rack, &expected_item->rack, sizeof(item->rack)) == 0);
    assert(item->count == expected_item->count);
    assert(item->alias == expected_item->alias);
    assert(item->probability == expected_item->probability);
  }

  const LeaveRackList *lrl = inference_results_get_leave_rack_list(results);
  const LeaveRackList *expected_lrl =
      inference_results_get_leave_rack_list(expected);
  assert(leave_rack_list_get_count(lrl) ==
         leave_rack_list_get_count(expected_lrl));
  for (int rack_idx = 0; rack_idx < leave_rack_list_get_count(lrl);
       rack_idx++) {
    assert(
        leave_rack_get_draws(leave_rack_list_get_rack(lrl, rack_idx)) ==
        leave_rack_get_draws(leave_rack_list_get_rack(expected_lrl, rack_idx)));
  }
}

static Config *create_config_for_threads_test(const int num_threads) {
  char *config_settings_str = get_formatted_string(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 "
      "all -numplays 20 -threads %d",
      num_threads);
  Config *config = config_create_or_die(config_settings_str);
  free(config_settings_str);
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  return config;
}

// Infers a tile placement with three unknown tiles (a few thousand possible
// leaves) or an exchange with three unknown tiles.
static void infer_for_threads_test(InferenceCtx **ctx, const Config *config,
                                   const bool exchange,
                                   InferenceResults *results) {
  error_code_t status;
  if (exchange) {
    status = infer_for_test(ctx, config, 0, 0, 3, "", "EIQU", "", results);
  } else {
    status = infer_for_test(ctx, config, 0, 20, 0, "HUJA", "", "", results);
  }
  assert(status == ERROR_STATUS_SUCCESS);
}

// Each possible leave is evaluated by exactly one of the workers, whichever
// claims it, so the inference counts the same leaves at any number of threads.
static void test_infer_same_results_for_any_number_of_threads(void) {
  Config *config = create_config_for_threads_test(1);
  InferenceResults *expected_play_results = inference_results_create(NULL);
  InferenceResults *expected_exchange_results = inference_results_create(NULL);
  infer_for_threads_test(NULL, config, false, expected_play_results);
  infer_for_threads_test(NULL, config, true, expected_exchange_results);
  config_destroy(config);
  assert(stat_get_num_unique_samples(inference_results_get_equity_values(
             expected_play_results, INFERENCE_TYPE_LEAVE)) > 0);
  assert(stat_get_num_unique_samples(inference_results_get_equity_values(
             expected_exchange_results, INFERENCE_TYPE_EXCHANGED)) > 0);

  const int thread_counts[] = {2, 16};
  const int num_thread_counts =
      (int)(sizeof(thread_counts) / sizeof(thread_counts[0]));
  for (int thread_count_idx = 0; thread_count_idx < num_thread_counts;
       thread_count_idx++) {
    config = create_config_for_threads_test(thread_counts[thread_count_idx]);
    // The second and third inferences reuse the context's workers, as
    // simulations do.
    InferenceCtx *ctx = NULL;
    InferenceResults *results = inference_results_create(NULL);
    infer_for_threads_test(&ctx, config, false, results);
    assert_inference_results_count_the_same(results, expected_play_results);
    infer_for_threads_test(&ctx, config, true, results);
    assert_inference_results_count_the_same(results, expected_exchange_results);
    infer_for_threads_test(&ctx, config, false, results);
    assert_inference_results_count_the_same(results, expected_play_results);
    inference_results_destroy(results);
    inference_ctx_destroy(ctx);
    config_destroy(config);
  }
  inference_results_destroy(expected_play_results);
  inference_results_destroy(expected_exchange_results);
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
  test_infer_exchange_margin();
  test_infer_exchange_cutoff_matches();
  for (int i = 0; i < 2; i++) {
    const bool use_game_history = i == 1;
    test_infer_nonerror_cases(1, use_game_history);
    test_infer_nonerror_cases(2, use_game_history);
    test_infer_nonerror_cases(7, use_game_history);
  }
  test_infer_same_results_for_any_number_of_threads();
}
