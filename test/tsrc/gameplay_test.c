#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

#include "../../src/def/game_defs.h"
#include "../../src/def/rack_defs.h"

#include "../../src/ent/bag.h"
#include "../../src/ent/board.h"
#include "../../src/ent/config.h"
#include "../../src/ent/game.h"
#include "../../src/ent/letter_distribution.h"
#include "../../src/ent/player.h"
#include "../../src/ent/rack.h"

#include "../../src/impl/gameplay.h"

#include "test_util.h"

void return_rack_to_bag(Rack *rack, Bag *bag, int player_draw_index) {
  for (int i = 0; i < (rack_get_dist_size(rack)); i++) {
    for (int j = 0; j < rack_get_letter(rack, i); j++) {
      bag_add_letter(bag, i, player_draw_index);
    }
  }
  rack_reset(rack);
}

void return_racks_to_bag(Game *game) {
  Bag *bag = game_get_bag(game);
  Player *player0 = game_get_player(game, 0);
  Player *player1 = game_get_player(game, 1);
  Rack *player0_rack = player_get_rack(player0);
  Rack *player1_rack = player_get_rack(player1);
  return_rack_to_bag(player0_rack, bag, game_get_player_draw_index(game, 0));
  return_rack_to_bag(player1_rack, bag, game_get_player_draw_index(game, 1));
}

void assert_players_are_equal(const Player *p1, const Player *p2,
                              bool check_scores) {
  // For games ending in consecutive zeros, scores are checked elsewhere
  if (check_scores) {
    assert(player_get_score(p1) == player_get_score(p2));
  }
}

void assert_games_are_equal(Game *g1, Game *g2, bool check_scores) {
  assert(game_get_consecutive_scoreless_turns(g1) ==
         game_get_consecutive_scoreless_turns(g2));
  assert(game_get_game_end_reason(g1) == game_get_game_end_reason(g2));

  int g1_player_on_turn_index = game_get_player_on_turn_index(g1);

  const Player *g1_player_on_turn =
      game_get_player(g1, g1_player_on_turn_index);
  const Player *g1_player_not_on_turn =
      game_get_player(g1, 1 - g1_player_on_turn_index);

  int g2_player_on_turn_index = game_get_player_on_turn_index(g2);

  const Player *g2_player_on_turn =
      game_get_player(g2, g2_player_on_turn_index);
  const Player *g2_player_not_on_turn =
      game_get_player(g2, 1 - g2_player_on_turn_index);

  assert_players_are_equal(g1_player_on_turn, g2_player_on_turn, check_scores);
  assert_players_are_equal(g1_player_not_on_turn, g2_player_not_on_turn,
                           check_scores);

  Bag *bag1 = game_get_bag(g1);
  Bag *bag2 = game_get_bag(g2);

  assert_boards_are_equal(game_get_board(g1, 0), game_get_board(g2, 0));
  assert_transposed_boards_are_equal(game_get_board(g1, 0),
                                     game_get_board(g1, 1));
  assert_boards_are_equal(game_get_board(g1, 1), game_get_board(g2, 1));
  assert_transposed_boards_are_equal(game_get_board(g2, 0),
                                     game_get_board(g2, 1));

  assert_bags_are_equal(bag1, bag2, ld_get_size(game_get_ld(g1)));
}

void test_gameplay_by_turn(const Config *config, char *cgps[], char *racks[],
                           int array_length) {
  Game *actual_game = game_create(config);
  Game *expected_game = game_create(config);

  int player0_last_score_on_rack = -1;
  int player1_last_score_on_rack = -1;
  int player0_final_score = -1;
  int player1_final_score = -1;
  int player0_score_before_last_move = -1;
  int player1_score_before_last_move = -1;

  for (int i = 0; i < array_length; i++) {
    assert(game_get_game_end_reason(actual_game) == GAME_END_REASON_NONE);
    return_racks_to_bag(actual_game);

    const LetterDistribution *ld = game_get_ld(actual_game);
    Bag *bag = game_get_bag(actual_game);

    int player_on_turn_index = game_get_player_on_turn_index(actual_game);
    int opponent_index = 1 - player_on_turn_index;
    Player *player_on_turn = game_get_player(actual_game, player_on_turn_index);
    Player *opponent = game_get_player(actual_game, 1 - player_on_turn_index);
    Rack *player_on_turn_rack = player_get_rack(player_on_turn);
    Rack *opponent_rack = player_get_rack(opponent);

    draw_rack_to_string(ld, bag, player_on_turn_rack, racks[i],
                        player_on_turn_index);
    // If it's the last turn, have the opponent draw the remaining tiles
    // so the end of actual_game subtractions are correct. If the bag has less
    // than RACK_SIZE tiles, have the opponent draw the remaining tiles
    // so the endgame adjustments are added to the move equity values.
    if (i == array_length - 1 || bag_get_tiles(bag) < RACK_SIZE) {
      draw_at_most_to_rack(
          bag, opponent_rack, RACK_SIZE,
          game_get_player_draw_index(actual_game, opponent_index));
    }

    Player *player0 = game_get_player(actual_game, 0);
    Player *player1 = game_get_player(actual_game, 1);

    Rack *player0_rack = player_get_rack(player0);
    Rack *player1_rack = player_get_rack(player1);

    if (i == array_length - 1) {
      player0_score_before_last_move = player_get_score(player0);
      player1_score_before_last_move = player_get_score(player1);
    }

    play_top_n_equity_move(actual_game, 0);

    if (i == array_length - 1) {
      player0_last_score_on_rack = rack_get_score(ld, player0_rack);
      player1_last_score_on_rack = rack_get_score(ld, player1_rack);
      player0_final_score = player_get_score(player0);
      player1_final_score = player_get_score(player1);
    }

    printf("%d, %d, %d: %d != %d\n", 7, 7, 0,
           board_get_anchor(game_get_board(expected_game, 1), 7, 7, 0),
           board_get_anchor(game_get_board(actual_game, 1), 7, 7, 0));

    game_load_cgp(expected_game, cgps[i]);

    printf("%d, %d, %d: %d != %d\n", 7, 7, 0,
           board_get_anchor(game_get_board(expected_game, 1), 7, 7, 0),
           board_get_anchor(game_get_board(actual_game, 1), 7, 7, 0));

    // If the game is still ongoing,
    // return the racks to the bag so that
    // the bag from the expected game and
    // the actual game match. If this is
    // the last position of a standard game, there
    // is no randomness for the rack draw
    // since there should be less than seven
    // tiles in the bag. If this is the last position
    // of a six pass game, we need to return the
    // tiles because those tiles could be random, and
    // so the bags probably won't match.
    if (i != array_length - 1 || game_get_game_end_reason(expected_game) ==
                                     GAME_END_REASON_CONSECUTIVE_ZEROS) {
      return_racks_to_bag(actual_game);
    }
    printf("cgp: %s\n", cgps[i]);
    print_game(expected_game, NULL);
    print_game(actual_game, NULL);
    assert_games_are_equal(expected_game, actual_game, i != array_length - 1);
  }

  if (game_get_game_end_reason(actual_game) ==
      GAME_END_REASON_CONSECUTIVE_ZEROS) {
    assert(player0_score_before_last_move - player0_last_score_on_rack ==
           player0_final_score);
    assert(player1_score_before_last_move - player1_last_score_on_rack ==
           player1_final_score);
  }

  game_destroy(actual_game);
  game_destroy(expected_game);
}

void test_draw_at_most_to_rack() {
  Config *config = create_config_or_die(
      "setoptions lex NWL20 s1 score s2 score r1 all r2 all numplays 1");
  const LetterDistribution *ld = config_get_ld(config);
  int ld_size = ld_get_size(ld);
  Bag *bag = bag_create(ld);
  Rack *rack = rack_create(ld_size);

  // Check drawing from the bag
  int drawing_player = 0;
  int number_of_remaining_tiles = bag_get_tiles(bag);

  while (bag_get_tiles(bag) > RACK_SIZE) {
    draw_at_most_to_rack(bag, rack, RACK_SIZE, drawing_player);
    drawing_player = 1 - drawing_player;
    number_of_remaining_tiles -= RACK_SIZE;
    assert(!rack_is_empty(rack));
    assert(rack_get_total_letters(rack) == RACK_SIZE);
    rack_reset(rack);
  }

  draw_at_most_to_rack(bag, rack, RACK_SIZE, drawing_player);
  assert(bag_is_empty(bag));
  assert(!rack_is_empty(rack));
  assert(rack_get_total_letters(rack) == number_of_remaining_tiles);
  rack_reset(rack);

  bag_destroy(bag);
  rack_destroy(rack);
  config_destroy(config);
}

void test_six_exchanges_game() {
  Config *config = create_config_or_die(
      "setoptions lex CSW21 s1 equity s2 equity r1 all r2 all numplays 1");

  char *racks[18] = {"UUUVVWW", "AEFRWYZ", "INOOQSU", "LUUUVVW", "EEEEEOO",
                     "AEIKLMO", "GNOOOPR", "EGIJLRS", "EEEOTTT", "EIILRSX",
                     "?CEEILT", "?AFERST", "AAAAAAI", "GUUUVVW", "AEEEEEO",
                     "AAAAAII", "AEUUUVV", "AEEEEII"};

  char *cgps[18] = {
      "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 1 lex CSW21;",
      "15/15/15/15/15/15/15/7FRAWZEY1/15/15/15/15/15/15/15 / 0/120 0 lex "
      "CSW21;",
      "15/15/15/14Q/14U/14I/14N/7FRAWZEYS/15/15/15/15/15/15/15 / 120/150 0 lex "
      "CSW21;",
      "15/15/15/14Q/14U/14I/14N/7FRAWZEYS/15/15/15/15/15/15/15 / 150/120 1 lex "
      "CSW21;",
      "15/15/15/14Q/14U/14I/14N/7FRAWZEYS/15/15/15/15/15/15/15 / 120/150 2 lex "
      "CSW21;",
      "15/15/15/14Q/14U/14I/14N/7FRAWZEYS/7O7/7A7/7M7/7L7/7I7/7K7/7E7 / "
      "150/224 0 lex CSW21;",
      "15/15/15/14Q/14U/14I/14N/7FRAWZEYS/7O7/7A7/7M7/7L7/7I7/7K7/GONOPORE7 / "
      "224/236 0 lex CSW21;",
      "15/15/15/14Q/14U/14I/14N/7FRAWZEYS/7O7/7A7/7M7/7L7/7I7/1JIG3K7/"
      "GONOPORE7 / 236/269 0 lex CSW21;",
      "15/15/15/14Q/14U/14I/14N/7FRAWZEYS/6TOETOE3/7A7/7M7/7L7/7I7/1JIG3K7/"
      "GONOPORE7 / 269/265 0 lex CSW21;",
      "15/15/15/14Q/14U/14I/14N/7FRAWZEYS/6TOETOE3/7A3XI2/7M7/7L7/7I7/1JIG3K7/"
      "GONOPORE7 / 265/297 0 lex CSW21;",
      "15/15/15/14Q/14U/14I/14N/7FRAWZEYS/6TOETOE3/7A3XI2/7M7/7L7/5ELICITEd2/"
      "1JIG3K7/GONOPORE7 / 297/341 0 lex CSW21;",
      "15/15/15/14Q/14U/14I/14N/7FRAWZEYS/6TOETOE3/7A3XI2/3FoREMAST4/7L7/"
      "5ELICITEd2/1JIG3K7/GONOPORE7 / 341/395 0 lex CSW21;",
      "15/15/15/14Q/14U/14I/14N/7FRAWZEYS/6TOETOE3/7A3XI2/3FoREMAST4/7L7/"
      "5ELICITEd2/1JIG3K7/GONOPORE7 / 395/341 1 lex CSW21;",
      "15/15/15/14Q/14U/14I/14N/7FRAWZEYS/6TOETOE3/7A3XI2/3FoREMAST4/7L7/"
      "5ELICITEd2/1JIG3K7/GONOPORE7 / 341/395 2 lex CSW21;",
      "15/15/15/14Q/14U/14I/14N/7FRAWZEYS/6TOETOE3/7A3XI2/3FoREMAST4/7L7/"
      "5ELICITEd2/1JIG3K7/GONOPORE7 / 395/341 3 lex CSW21;",
      "15/15/15/14Q/14U/14I/14N/7FRAWZEYS/6TOETOE3/7A3XI2/3FoREMAST4/7L7/"
      "5ELICITEd2/1JIG3K7/GONOPORE7 / 341/395 4 lex CSW21;",
      "15/15/15/14Q/14U/14I/14N/7FRAWZEYS/6TOETOE3/7A3XI2/3FoREMAST4/7L7/"
      "5ELICITEd2/1JIG3K7/GONOPORE7 / 395/341 5 lex CSW21;",
      "15/15/15/14Q/14U/14I/14N/7FRAWZEYS/6TOETOE3/7A3XI2/3FoREMAST4/7L7/"
      "5ELICITEd2/1JIG3K7/GONOPORE7 / 332/384 6 lex CSW21;"};
  test_gameplay_by_turn(config, cgps, racks, 18);
  config_destroy(config);
}

void test_six_passes_game() {
  Config *config = create_config_or_die(
      "setoptions lex CSW21 s1 equity s2 equity r1 all r2 all numplays 1");

  char *racks[31] = {"AEGILPR", "ACELNTV", "DDEIOTY", "?ADIIUU", "?BEIINS",
                     "EEEKMNO", "AAEHINT", "CDEGORZ", "EGNOQRS", "AFIQRRT",
                     "ERSSTTX", "BGHNOOU", "AENRTUZ", "AFIMNRV", "AEELNOT",
                     "?EORTUW", "ILNOOST", "EEINRUY", "?AENRTU", "EEINRUW",
                     "AJNPRV",  "INRU",    "PRV",     "U",       "RV",
                     "U",       "V",       "U",       "V",       "U",
                     "V"};

  char *cgps[31] = {
      "15/15/15/15/15/15/15/3PAIGLE6/15/15/15/15/15/15/15 / 0/24 0 lex CSW21;",
      "15/15/15/15/4V10/4A10/4L10/3PAIGLE6/4N10/4C10/4E10/15/15/15/15 / 24/48 "
      "0 lex CSW21;",
      "15/15/15/15/4V10/4A10/4L10/3PAIGLE6/4N10/4C10/4E10/2ODDITY7/15/15/15 / "
      "48/68 0 lex CSW21;",
      "15/15/15/15/4V10/4A10/4L10/3PAIGLE6/4N10/4C10/4E10/2ODDITY7/1DUI11/15/"
      "15 / 68/63 0 lex CSW21;",
      "15/15/15/15/4V10/4A10/4L10/3PAIGLE6/4N10/4C10/4EBIoNISE3/2ODDITY7/"
      "1DUI11/15/15 / 63/146 0 lex CSW21;",
      "15/15/15/15/4V10/4A10/4L10/3PAIGLE6/4N10/4C2MOKE4/4EBIoNISE3/2ODDITY7/"
      "1DUI11/15/15 / 146/110 0 lex CSW21;",
      "15/15/15/15/4V10/4A10/4L1AAH6/3PAIGLE6/4N10/4C2MOKE4/4EBIoNISE3/"
      "2ODDITY7/1DUI11/15/15 / 110/172 0 lex CSW21;",
      "15/15/15/15/4V10/4A10/4L1AAH6/3PAIGLE6/4N10/4C2MOKE4/4EBIoNISE3/"
      "2ODDITY7/1DUI11/CODGER9/15 / 172/149 0 lex CSW21;",
      "15/15/15/15/4V10/4A10/4L1AAH6/3PAIGLE6/4N10/4C2MOKE4/4EBIoNISE3/"
      "2ODDITY7/1DUI11/CODGER9/15 / 149/172 1 lex CSW21;",
      "15/15/15/15/4V10/4A10/4L1AAH6/3PAIGLE6/4N5FAQIR/4C2MOKE4/4EBIoNISE3/"
      "2ODDITY7/1DUI11/CODGER9/15 / 172/182 0 lex CSW21;",
      "15/15/15/15/4V10/4A6T3/4L1AAH2E3/3PAIGLE2X3/4N5FAQIR/4C2MOKES3/"
      "4EBIoNISE3/2ODDITY3S3/1DUI11/CODGER9/15 / 182/227 0 lex CSW21;",
      "15/15/15/15/4V10/4A6T3/4L1AAH2E2B/3PAIGLE2X2O/4N5FAQIR/4C2MOKES2O/"
      "4EBIoNISE2U/2ODDITY3S2G/1DUI10H/CODGER9/15 / 227/227 0 lex CSW21;",
      "15/15/15/15/4V10/4A6T3/4L1AAH2E2B/3PAIGLE2X1TO/4N5FAQIR/4C2MOKES1ZO/"
      "4EBIoNISE2U/2ODDITY3S2G/1DUI10H/CODGER9/15 / 227/292 0 lex CSW21;",
      "15/15/12F2/12R2/4V7A2/4A6TI2/4L1AAH2EM1B/3PAIGLE2X1TO/4N5FAQIR/"
      "4C2MOKES1ZO/4EBIoNISE2U/2ODDITY3S2G/1DUI10H/CODGER9/15 / 292/262 0 lex "
      "CSW21;",
      "15/15/12F2/12R2/4V7A2/4A6TI2/4L1AAH2EM1B/3PAIGLE2X1TO/4N5FAQIR/"
      "4C2MOKES1ZO/4EBIoNISE2U/2ODDITY3S2G/1DUI1EALE5H/CODGER9/15 / 262/316 0 "
      "lex CSW21;",
      "15/15/12F2/11TROW/4V7A2/4A6TI2/4L1AAH2EM1B/3PAIGLE2X1TO/4N5FAQIR/"
      "4C2MOKES1ZO/4EBIoNISE2U/2ODDITY3S2G/1DUI1EALE5H/CODGER9/15 / 316/284 0 "
      "lex CSW21;",
      "15/15/12F2/11TROW/4V7A2/4A6TI2/4L1AAH2EM1B/3PAIGLE2X1TO/4N5FAQIR/"
      "4C2MOKES1ZO/4EBIoNISE2U/2ODDITY3S2G/1DUI1EALE5H/CODGER2LOTIONS/15 / "
      "284/400 0 lex CSW21;",
      "15/15/12F2/11TROW/4V7A2/4A6TI2/4L1AAH2EM1B/3PAIGLE2X1TO/4N5FAQIR/"
      "4C2MOKES1ZO/4EBIoNISE2U/2ODDITY3S2G/1DUI1EALE3YEH/CODGER2LOTIONS/15 / "
      "400/314 0 lex CSW21;",
      "15/15/12F2/11TROW/4V7A2/2iNAURATE1TI2/4L1AAH2EM1B/3PAIGLE2X1TO/4N5FAQIR/"
      "4C2MOKES1ZO/4EBIoNISE2U/2ODDITY3S2G/1DUI1EALE3YEH/CODGER2LOTIONS/15 / "
      "314/474 0 lex CSW21;",
      "15/15/12F2/11TROW/4V3EWE1A2/2iNAURATE1TI2/4L1AAH2EM1B/3PAIGLE2X1TO/"
      "4N5FAQIR/4C2MOKES1ZO/4EBIoNISE2U/2ODDITY3S2G/1DUI1EALE3YEH/"
      "CODGER2LOTIONS/15 / 474/338 0 lex CSW21;",
      "15/15/12F2/11TROW/4V3EWE1A2/2iNAURATE1TI2/4L1AAH2EM1B/3PAIGLE2X1TO/"
      "2JANN4FAQIR/4C2MOKES1ZO/4EBIoNISE2U/2ODDITY3S2G/1DUI1EALE3YEH/"
      "CODGER2LOTIONS/15 / 338/499 0 lex CSW21;",
      "15/15/12F2/11TROW/4V3EWE1A2/2iNAURATE1TI2/4L1AAH2EM1B/3PAIGLE2X1TO/"
      "2JANN4FAQIR/4C2MOKES1ZO/4EBIoNISE2U/2ODDITY3S2G/1DUI1EALE3YEH/"
      "CODGER2LOTIONS/9RIN3 / 499/349 0 lex CSW21;",
      "15/15/12F2/11TROW/4V3EWE1A2/2iNAURATE1TIP1/4L1AAH2EM1B/3PAIGLE2X1TO/"
      "2JANN4FAQIR/4C2MOKES1ZO/4EBIoNISE2U/2ODDITY3S2G/1DUI1EALE3YEH/"
      "CODGER2LOTIONS/9RIN3 / 349/510 0 lex CSW21;",
      "15/15/12F2/11TROW/4V3EWE1A2/2iNAURATE1TIP1/4L1AAH2EM1B/3PAIGLE2X1TO/"
      "2JANN4FAQIR/4C2MOKES1ZO/4EBIoNISE2U/2ODDITY3S2G/1DUI1EALE3YEH/"
      "CODGER2LOTIONS/9RIN3 / 510/349 1 lex CSW21;",
      "15/15/12F2/11TROW/4V3EWE1A2/2iNAURATE1TIP1/4L1AAH2EM1B/3PAIGLE2X1TO/"
      "2JANN4FAQIR/4C2MOKES1ZO/4EBIoNISE2U/2ODDITY1R1S2G/1DUI1EALE3YEH/"
      "CODGER2LOTIONS/9RIN3 / 349/517 0 lex CSW21;",
      "15/15/12F2/11TROW/4V3EWE1A2/2iNAURATE1TIP1/4L1AAH2EM1B/3PAIGLE2X1TO/"
      "2JANN4FAQIR/4C2MOKES1ZO/4EBIoNISE2U/2ODDITY1R1S2G/1DUI1EALE3YEH/"
      "CODGER2LOTIONS/9RIN3 / 517/349 1 lex CSW21;",
      "15/15/12F2/11TROW/4V3EWE1A2/2iNAURATE1TIP1/4L1AAH2EM1B/3PAIGLE2X1TO/"
      "2JANN4FAQIR/4C2MOKES1ZO/4EBIoNISE2U/2ODDITY1R1S2G/1DUI1EALE3YEH/"
      "CODGER2LOTIONS/9RIN3 / 349/517 2 lex CSW21;",
      "15/15/12F2/11TROW/4V3EWE1A2/2iNAURATE1TIP1/4L1AAH2EM1B/3PAIGLE2X1TO/"
      "2JANN4FAQIR/4C2MOKES1ZO/4EBIoNISE2U/2ODDITY1R1S2G/1DUI1EALE3YEH/"
      "CODGER2LOTIONS/9RIN3 / 517/349 3 lex CSW21;",
      "15/15/12F2/11TROW/4V3EWE1A2/2iNAURATE1TIP1/4L1AAH2EM1B/3PAIGLE2X1TO/"
      "2JANN4FAQIR/4C2MOKES1ZO/4EBIoNISE2U/2ODDITY1R1S2G/1DUI1EALE3YEH/"
      "CODGER2LOTIONS/9RIN3 / 349/517 4 lex CSW21;",
      "15/15/12F2/11TROW/4V3EWE1A2/2iNAURATE1TIP1/4L1AAH2EM1B/3PAIGLE2X1TO/"
      "2JANN4FAQIR/4C2MOKES1ZO/4EBIoNISE2U/2ODDITY1R1S2G/1DUI1EALE3YEH/"
      "CODGER2LOTIONS/9RIN3 / 517/349 5 lex CSW21;",
      "15/15/12F2/11TROW/4V3EWE1A2/2iNAURATE1TIP1/4L1AAH2EM1B/3PAIGLE2X1TO/"
      "2JANN4FAQIR/4C2MOKES1ZO/4EBIoNISE2U/2ODDITY1R1S2G/1DUI1EALE3YEH/"
      "CODGER2LOTIONS/9RIN3 / 348/513 6 lex CSW21;"};

  test_gameplay_by_turn(config, cgps, racks, 31);
  config_destroy(config);
}

void test_standard_game() {
  Config *config = create_config_or_die(
      "setoptions lex CSW21 s1 equity s2 equity r1 all r2 all numplays 1");

  char *racks[23] = {"EGIILNO", "DRRTYYZ", "CEIOTTU", "AADEEMT", "AACDEKS",
                     "BEEIOOP", "DHLNORR", "BGIIJRV", "?DFMNPU", "EEEOQRW",
                     "IINNSVW", "?ADEOPU", "EFOTTUV", "ADHIMNX", "CEFINQS",
                     "?ADRSTT", "?CIRRSU", "AEEFGIL", "EEGHLMN", "AAAEELL",
                     "DEEGLNN", "AEGILUY", "EN"};

  // Assign the rack on the last CGP so that the CGP load
  // function ends the game.
  char *cgps[23] = {
      "15/15/15/15/15/15/15/6OILING3/15/15/15/15/15/15/15 / 0/18 0 lex CSW21;",
      "15/15/15/15/15/15/9R5/6OILING3/9T5/9Z5/9Y5/15/15/15/15 / 18/37 0 lex "
      "CSW21;",
      "15/15/15/15/15/15/9R5/6OILING3/9T5/9ZO4/9YU4/10T4/15/15/15 / 37/45 0 "
      "lex CSW21;",
      "15/15/15/15/15/15/9R5/6OILING3/9T5/9ZO4/9YU4/10T4/6EDEMATA2/15/15 / "
      "45/115 0 lex CSW21;",
      "15/15/15/15/15/15/9R5/6OILING3/9T5/9ZOS3/9YUK3/10TA3/6EDEMATA2/11E3/"
      "11D3 / 115/97 0 lex CSW21;",
      "15/15/15/15/15/15/9R5/6OILING3/9T5/7B1ZOS3/7O1YUK3/7O2TA3/6EDEMATA2/"
      "7I3E3/7E3D3 / 97/145 0 lex CSW21;",
      "15/15/15/15/15/15/9R5/6OILING3/9T5/7B1ZOS3/7O1YUK3/7O2TA3/6EDEMATA2/"
      "5RHINO1E3/7E3D3 / 145/122 0 lex CSW21;",
      "15/15/15/15/15/5J9/5I3R5/5BOILING3/9T5/7B1ZOS3/7O1YUK3/7O2TA3/6EDEMATA2/"
      "5RHINO1E3/7E3D3 / 122/183 0 lex CSW21;",
      "15/15/15/15/15/5J9/5IF2R5/5BOILING3/6P2T5/7B1ZOS3/7O1YUK3/7O2TA3/"
      "6EDEMATA2/5RHINO1E3/7E3D3 / 183/146 0 lex CSW21;",
      "15/15/15/15/15/5J9/5IF2R5/5BOILING3/6P2T5/5R1B1ZOS3/5E1O1YUK3/5W1O2TA3/"
      "5OEDEMATA2/5RHINO1E3/5E1E3D3 / 146/205 0 lex CSW21;",
      "15/15/11W3/11I3/11V3/5J5I3/5IF2R1N3/5BOILING3/6P2T5/5R1B1ZOS3/5E1O1YUK3/"
      "5W1O2TA3/5OEDEMATA2/5RHINO1E3/5E1E3D3 / 205/172 0 lex CSW21;",
      "15/15/11W3/11I3/11V3/5J2P2I3/5IF1UR1N3/5BOILING3/6P1AT5/5R1BOZOS3/"
      "5E1O1YUK3/5W1O2TA3/5OEDEMATA2/5RHINO1E3/5E1E3D3 / 172/236 0 lex CSW21;",
      "15/15/11W3/11I3/11V3/5J2P2I3/5IF1UR1N3/5BOILING3/6P1AT5/5R1BOZOS3/"
      "5E1O1YUKO2/5W1O2TAV2/5OEDEMATA2/5RHINO1ET2/5E1E3DE2 / 236/202 0 lex "
      "CSW21;",
      "15/15/11W3/11I3/11V3/5J2P2I3/5IF1UR1N3/5BOILING3/6P1AT5/5R1BOZOS3/"
      "5E1O1YUKO2/5W1O2TAV2/5OEDEMATA2/5RHINO1ETA1/5E1E3DEX1 / 202/271 0 lex "
      "CSW21;",
      "15/15/11W3/11I3/11V3/5J2P2I3/5IF1UR1N1C1/5BOILING1I1/6P1AT3N1/"
      "5R1BOZOS1Q1/5E1O1YUKOS1/5W1O2TAV2/5OEDEMATA2/5RHINO1ETA1/5E1E3DEX1 / "
      "271/250 0 lex CSW21;",
      "15/5TeTRADS3/11W3/11I3/11V3/5J2P2I3/5IF1UR1N1C1/5BOILING1I1/6P1AT3N1/"
      "5R1BOZOS1Q1/5E1O1YUKOS1/5W1O2TAV2/5OEDEMATA2/5RHINO1ETA1/5E1E3DEX1 / "
      "250/346 0 lex CSW21;",
      "1CURRIeS7/5TeTRADS3/11W3/11I3/11V3/5J2P2I3/5IF1UR1N1C1/5BOILING1I1/"
      "6P1AT3N1/5R1BOZOS1Q1/5E1O1YUKOS1/5W1O2TAV2/5OEDEMATA2/5RHINO1ETA1/"
      "5E1E3DEX1 / 346/335 0 lex CSW21;",
      "1CURRIeS7/5TeTRADS3/11W3/11I3/11V3/5J2P2I3/5IF1UR1N1C1/5BOILING1IF/"
      "6P1AT3NE/5R1BOZOS1Q1/5E1O1YUKOS1/5W1O2TAV2/5OEDEMATA2/5RHINO1ETA1/"
      "5E1E3DEX1 / 335/378 0 lex CSW21;",
      "1CURRIeS7/1HM2TeTRADS3/11W3/11I3/11V3/5J2P2I3/5IF1UR1N1C1/5BOILING1IF/"
      "6P1AT3NE/5R1BOZOS1Q1/5E1O1YUKOS1/5W1O2TAV2/5OEDEMATA2/5RHINO1ETA1/"
      "5E1E3DEX1 / 378/367 0 lex CSW21;",
      "1CURRIeS7/1HM2TeTRADS3/11W3/11I3/10AVALE/5J2P2I3/5IF1UR1N1C1/"
      "5BOILING1IF/6P1AT3NE/5R1BOZOS1Q1/5E1O1YUKOS1/5W1O2TAV2/5OEDEMATA2/"
      "5RHINO1ETA1/5E1E3DEX1 / 367/394 0 lex CSW21;",
      "1CURRIeS6L/1HM2TeTRADS2E/11W2N/11I2G/10AVALE/5J2P2I2D/5IF1UR1N1C1/"
      "5BOILING1IF/6P1AT3NE/5R1BOZOS1Q1/5E1O1YUKOS1/5W1O2TAV2/5OEDEMATA2/"
      "5RHINO1ETA1/5E1E3DEX1 / 394/397 0 lex CSW21;",
      "1CURRIeS6L/1HM2TeTRADS2E/11W2N/11I2G/10AVALE/5J2P2I2D/5IF1UR1N1C1/"
      "5BOILING1IF/6P1AT3NE/5R1BOZOS1Q1/5E1O1YUKOS1/5W1O2TAV2/5OEDEMATA2/"
      "5RHINO1ETA1/5E1E3DEXY / 397/439 0 lex CSW21;",
      "1CURRIeS6L/1HM2TeTRADS2E/2EN7W2N/11I2G/10AVALE/5J2P2I2D/5IF1UR1N1C1/"
      "5BOILING1IF/6P1AT3NE/5R1BOZOS1Q1/5E1O1YUKOS1/5W1O2TAV2/5OEDEMATA2/"
      "5RHINO1ETA1/5E1E3DEXY AEGILU/ 439/425 0 lex CSW21;"};

  test_gameplay_by_turn(config, cgps, racks, 23);
  config_destroy(config);
}

void test_playmove() {
  Config *config = create_config_or_die(
      "setoptions lex CSW21 s1 equity s2 equity r1 all r2 all numplays 1");
  Game *game = game_create(config);
  Board *board = game_get_board(game, 0);
  Bag *bag = game_get_bag(game);
  const LetterDistribution *ld = game_get_ld(game);

  Player *player0 = game_get_player(game, 0);
  Player *player1 = game_get_player(game, 1);

  Rack *player0_rack = player_get_rack(player0);
  Rack *player1_rack = player_get_rack(player1);

  // Test play
  draw_rack_to_string(ld, bag, player0_rack, "DEKNRTY", 0);
  play_top_n_equity_move(game, 0);

  assert(game_get_consecutive_scoreless_turns(game) == 0);
  assert(game_get_game_end_reason(game) == GAME_END_REASON_NONE);
  assert(player_get_score(player0) == 36);
  assert(player_get_score(player1) == 0);
  assert(!rack_is_empty(player0_rack));
  assert(rack_get_total_letters(player0_rack) == 7);
  assert(board_get_letter(board, 7, 3) == ld_hl_to_ml(ld, "K"));
  assert(board_get_letter(board, 7, 4) == ld_hl_to_ml(ld, "Y"));
  assert(board_get_letter(board, 7, 5) == ld_hl_to_ml(ld, "N"));
  assert(board_get_letter(board, 7, 6) == ld_hl_to_ml(ld, "D"));
  assert(board_get_letter(board, 7, 7) == ld_hl_to_ml(ld, "E"));
  assert(game_get_player_on_turn_index(game) == 1);
  assert(bag_get_tiles(bag) == 88);
  assert(board_get_tiles_played(board) == 5);
  game_reset(game);

  // Test exchange
  draw_rack_to_string(ld, bag, player0_rack, "UUUVVWW", 0);
  play_top_n_equity_move(game, 0);

  assert(game_get_consecutive_scoreless_turns(game) == 1);
  assert(game_get_game_end_reason(game) == GAME_END_REASON_NONE);
  assert(player_get_score(player0) == 0);
  assert(player_get_score(player1) == 0);
  assert(!rack_is_empty(player0_rack));
  assert(rack_get_total_letters(player0_rack) == 7);
  assert(game_get_player_on_turn_index(game) == 1);
  assert(bag_get_tiles(bag) == 93);
  assert(board_get_tiles_played(board) == 0);

  assert(rack_get_letter(player0_rack, ld_hl_to_ml(ld, "V")) == 0);
  assert(rack_get_letter(player0_rack, ld_hl_to_ml(ld, "W")) == 0);
  assert(rack_get_letter(player0_rack, ld_hl_to_ml(ld, "U")) < 2);

  // Test pass
  game_load_cgp(
      game, "15/15/12F2/11TROW/4V3EWE1A2/2iNAURATE1TIP1/4L1AAH2EM1B/"
            "3PAIGLE2X1TO/2JANN4FAQIR/4C2MOKES1ZO/4EBIoNISE2U/2ODDITY1R1S2G/"
            "1DUI1EALE3YEH/CODGER2LOTIONS/9RIN3 / 517/349 5 lex CSW21;");
  draw_at_most_to_rack(bag, player0_rack, 1,
                       game_get_player_draw_index(game, 0));
  draw_at_most_to_rack(bag, player1_rack, 1,
                       game_get_player_draw_index(game, 1));

  int player0_score = player_get_score(player0);
  int player1_score = player_get_score(player1);
  assert(game_get_consecutive_scoreless_turns(game) == 5);
  assert(game_get_game_end_reason(game) == GAME_END_REASON_NONE);
  assert(player0_score == 517);
  assert(player1_score == 349);
  assert(!rack_is_empty(player0_rack));
  assert(rack_get_total_letters(player0_rack) == 1);
  assert(!rack_is_empty(player1_rack));
  assert(rack_get_total_letters(player1_rack) == 1);
  assert(game_get_player_on_turn_index(game) == 0);
  assert(bag_is_empty(bag));

  play_top_n_equity_move(game, 0);

  assert(game_get_consecutive_scoreless_turns(game) == 6);
  assert(game_get_game_end_reason(game) == GAME_END_REASON_CONSECUTIVE_ZEROS);
  assert(player_get_score(player0) ==
         player0_score - rack_get_score(ld, player0_rack));
  assert(player_get_score(player1) ==
         player1_score - rack_get_score(ld, player1_rack));
  assert(!rack_is_empty(player0_rack));
  assert(rack_get_total_letters(player0_rack) == 1);
  assert(rack_get_total_letters(player1_rack) == 1);
  assert(game_get_player_on_turn_index(game) == 0);
  assert(bag_is_empty(bag));

  game_destroy(game);
  config_destroy(config);
}

void test_set_random_rack() {
  Config *config = create_config_or_die(
      "setoptions lex CSW21 s1 equity s2 equity r1 all r2 all numplays 1");
  Game *game = game_create(config);

  Bag *bag = game_get_bag(game);
  const LetterDistribution *ld = game_get_ld(game);

  Player *player0 = game_get_player(game, 0);

  Rack *player0_rack = player_get_rack(player0);

  assert(bag_get_tiles(bag) == 100);
  // draw some random rack.
  draw_rack_to_string(ld, bag, player0_rack, "DEKNRTY", 0);
  assert(bag_get_tiles(bag) == 93);

  set_random_rack(game, 0, NULL);
  assert(bag_get_tiles(bag) == 93);
  assert(rack_get_total_letters(player0_rack) == 7);

  // draw some random rack, but with 5 fixed tiles
  Rack *known_rack = rack_create(ld_get_size(ld));
  rack_set_to_string(ld, known_rack, "CESAR");
  set_random_rack(game, 0, known_rack);
  assert(bag_get_tiles(bag) == 93);
  assert(rack_get_total_letters(player0_rack) == 7);

  // C E S A R
  rack_take_letter(player0_rack, 3);
  rack_take_letter(player0_rack, 5);
  rack_take_letter(player0_rack, 19);
  rack_take_letter(player0_rack, 1);
  rack_take_letter(player0_rack, 18);

  assert(!rack_is_empty(player0_rack));
  assert(rack_get_total_letters(player0_rack) == 2);
  // ensure the rack isn't corrupt
  int ct = 0;
  for (int i = 0; i < rack_get_dist_size(player0_rack); i++) {
    ct += rack_get_letter(player0_rack, i);
  }
  assert(ct == 2);

  rack_destroy(known_rack);
  game_destroy(game);
  config_destroy(config);
}

void test_backups() {
  Config *config = create_config_or_die(
      "setoptions lex CSW21 s1 equity s2 equity r1 all r2 all numplays 1");
  Game *game = game_create(config);
  Board *board = game_get_board(game, 0);
  Bag *bag = game_get_bag(game);
  const LetterDistribution *ld = game_get_ld(game);

  Player *player0 = game_get_player(game, 0);
  Player *player1 = game_get_player(game, 1);

  Rack *player0_rack = player_get_rack(player0);
  Rack *player1_rack = player_get_rack(player1);

  // draw some random rack.
  draw_rack_to_string(ld, bag, player0_rack, "DEKNRTY", 0);

  draw_rack_to_string(ld, bag, player1_rack, "AOQRTUZ", 1);
  assert(bag_get_tiles(bag) == 86);

  // backup
  game_set_backup_mode(game, BACKUP_MODE_SIMULATION);
  play_top_n_equity_move(game, 0);
  play_top_n_equity_move(game, 0);
  assert(bag_get_tiles(bag) == 74);

  assert(player_get_score(player0) == 36);
  assert(player_get_score(player1) == 131);
  assert(board_get_letter(board, 0, 7) == ld_hl_to_ml(ld, "Q"));
  // let's unplay QUATORZE
  game_unplay_last_move(game);
  assert(player_get_score(player0) == 36);
  assert(player_get_score(player1) == 0);

  assert(rack_get_letter(player1_rack, ld_hl_to_ml(ld, "A")) == 1);
  assert(rack_get_letter(player1_rack, ld_hl_to_ml(ld, "O")) == 1);
  assert(rack_get_letter(player1_rack, ld_hl_to_ml(ld, "Q")) == 1);
  assert(rack_get_letter(player1_rack, ld_hl_to_ml(ld, "R")) == 1);
  assert(rack_get_letter(player1_rack, ld_hl_to_ml(ld, "T")) == 1);
  assert(rack_get_letter(player1_rack, ld_hl_to_ml(ld, "U")) == 1);
  assert(rack_get_letter(player1_rack, ld_hl_to_ml(ld, "Z")) == 1);

  assert(board_get_letter(board, 0, 7) == 0);
  // was 85 after drawing racks for both players, then was 80 after KYNDE
  // and drawing 5 replacement tiles, then 73 after QUATORZ(E) and 7 replacement
  // tiles, then back to 80 after unplay.
  assert(bag_get_tiles(bag) == 81);
  game_destroy(game);
  config_destroy(config);
}

void test_gameplay() {
  test_draw_at_most_to_rack();
  test_playmove();
  test_six_exchanges_game();
  test_six_passes_game();
  test_standard_game();
  test_set_random_rack();
  test_backups();
}