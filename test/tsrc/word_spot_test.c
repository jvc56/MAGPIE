#include <assert.h>

#include "../../src/impl/config.h"
#include "../../src/impl/gameplay.h"

#include "test_constants.h"
#include "test_util.h"

void test_opening_racks(void) {
  Config *config = config_create_or_die("set -lex CSW21 -wmp true");
  Game *game = config_game_create(config);
  Player *player = game_get_player(game, 0);
  player_set_move_sort_type(player, MOVE_SORT_SCORE);
  MoveList *move_list = move_list_create(10);

  WordSpotHeap spot_list;
  load_and_build_spots(game, EMPTY_CGP, "MUZJIKS", &spot_list, move_list);
  int expected_count = 7 + 6 + 5 + 4 + 3 + 2;
  assert(spot_list.count == expected_count);
  for (int i = 0; i < expected_count; i++) {
    assert(spot_list.spots[i].row == 7);
    assert(spot_list.spots[i].dir == BOARD_HORIZONTAL_DIRECTION);
  }
  const WordSpot *top_muzjiks_spot = &spot_list.spots[0];
  assert_spot_equity_int(top_muzjiks_spot, 128);
  assert(top_muzjiks_spot->row == 7);
  assert(top_muzjiks_spot->num_tiles == 7);
  // Worst bingo spot would be an 8E placement missing the DLS
  const WordSpot *muzjiks_airball = &spot_list.spots[6];
  assert_spot_equity_int(muzjiks_airball, 108);
  assert(muzjiks_airball->row == 7);
  assert(muzjiks_airball->col == 4);
  assert(muzjiks_airball->num_tiles == 7);
  // Worst spot is a two-tile play
  const WordSpot *worst_muzjiks_spot = &spot_list.spots[expected_count - 1];
  assert_spot_equity_int(worst_muzjiks_spot, 36); // ZJ
  assert(worst_muzjiks_spot->num_tiles == 2);

  load_and_build_spots(game, EMPTY_CGP, "TRONGLE", &spot_list, move_list);
  expected_count = 6 + 5 + 4 + 3 + 2; // 7-letter word is not possible
  assert(spot_list.count == expected_count);
  const WordSpot *top_trongle_spot = &spot_list.spots[0];
  // We know there are sixes, and assume something could put the G on
  // the DWS even though none do.
  assert_spot_equity_int(top_trongle_spot, 18);

  load_and_build_spots(game, EMPTY_CGP, "VVWWXYZ", &spot_list, move_list);
  // WordSpot logic handles one-tile plays differently than shadow. We do not
  // record a spot for bogus one-tile words. One tile plays only form spots when
  // playing through tiles (and only a mainword direction).
  assert(spot_list.count == 0);

  game_destroy(game);
  config_destroy(config);
}

void test_best_leaves(void) {
  Config *config = config_create_or_die("set -lex CSW21 -wmp true");
  Game *game = config_game_create(config);
  Player *player = game_get_player(game, 0);
  player_set_move_sort_type(player, MOVE_SORT_EQUITY);
  MoveList *move_list = move_list_create(1);

  WordSpotHeap spot_list;
  load_and_build_spots(game, EMPTY_CGP, "QUACKY?", &spot_list, move_list);
  int expected_count = 6 + 5 + 4 + 3 + 2;
  assert(spot_list.count == expected_count);
  const WordSpot *top_quacky_spot = &spot_list.spots[0];
  const Equity quacky_score = int_to_equity(68);
  const Equity blank_leave_value = get_leave_value(game, "?");
  assert(top_quacky_spot->num_tiles == 6);
  assert(top_quacky_spot->best_possible_equity ==
         quacky_score + blank_leave_value);

  move_list_destroy(move_list);
  game_destroy(game);
  config_destroy(config);
}

void test_zerk(void) {
  Config *config = config_create_or_die("set -lex CSW21 -wmp true");
  Game *game = config_game_create(config);
  Player *player = game_get_player(game, 0);
  player_set_move_sort_type(player, MOVE_SORT_EQUITY);
  MoveList *move_list = move_list_create(1);

  WordSpotHeap spot_list;
  load_and_build_spots(game, EMPTY_CGP, "DEKNRXZ", &spot_list, move_list);
  int expected_count = 4 + 3 + 2;
  assert(spot_list.count == expected_count);
  const WordSpot *four_tile_spot = &spot_list.spots[0];
  assert(four_tile_spot->num_tiles == 4);
  const Equity dkxz_score = int_to_equity(50);
  const Equity nxz_leave_value = get_leave_value(game, "NXZ");
  assert(four_tile_spot->best_possible_score == dkxz_score);
  assert(four_tile_spot->best_possible_equity == dkxz_score + nxz_leave_value);

  move_list_destroy(move_list);
  game_destroy(game);
  config_destroy(config);
}

void test_bingos_after_vac(void) {
  // Board contains 8G VAC
  char vac[300] =
      "15/15/15/15/15/15/15/6VAC6/15/15/15/15/15/15/15 / 0/0 0 lex NWL20;";
  Config *config = config_create_or_die("set -l1 NWL20 -l2 CSW21 -wmp true");
  Game *game = config_game_create(config);
  Player *player = game_get_player(game, 0);
  MoveList *move_list = move_list_create(1);
  player_set_move_sort_type(player, MOVE_SORT_EQUITY);

  WordSpotHeap spot_list;
  // Player 1: NWL
  load_and_build_spots(game, vac, "ORATES?", &spot_list, move_list);
  // Bingo through VAC to the TWS
  WordSpot *top_orates_spot = &spot_list.spots[0];
  assert_spot_equity_int(top_orates_spot, 95);
  assert(top_orates_spot->row == 7);
  assert(top_orates_spot->num_tiles == 7);

  bool hooks_vac = false;
  bool underlaps_vac = false;
  for (int i = 0; i < spot_list.count; i++) {
    const WordSpot *spot = &spot_list.spots[i];
    if (spot->col == 9 && spot->dir == BOARD_VERTICAL_DIRECTION &&
        spot->num_tiles == 7) {
      hooks_vac = true;
    }
    if (spot->row == 8 && spot->dir == BOARD_HORIZONTAL_DIRECTION &&
        spot->num_tiles == 7) {
      underlaps_vac = true;
    }
  }
  assert(hooks_vac);
  assert(!underlaps_vac);

  Move *move = move_create();
  move_set_as_pass(move);
  play_move(move, game, NULL, NULL);
  move_destroy(move);

  // Player 2: CSW
  // The only difference is that because of CH, the CSW player is able to
  // underlap VAC with a bingo. Currently this would be allowed even if the
  // rack had no H or ? on it. Hooks are checked when creating BoardSpots, but
  // not yet restricted further when creating WordSpots as they are when
  // creating Anchors in gen_shadow.
  assert(game_get_player_on_turn_index(game) == 1);
  build_spots_for_current_position(game, "ORATES?", &spot_list, move_list);
  // Bingo through VAC to the TWS
  top_orates_spot = &spot_list.spots[0];
  assert_spot_equity_int(top_orates_spot, 95);
  assert(top_orates_spot->row == 7);
  assert(top_orates_spot->num_tiles == 7);

  hooks_vac = false;
  underlaps_vac = false;
  for (int i = 0; i < spot_list.count; i++) {
    const WordSpot *spot = &spot_list.spots[i];
    if (spot->col == 9 && spot->dir == BOARD_VERTICAL_DIRECTION &&
        spot->num_tiles == 7) {
      hooks_vac = true;
    }
    if (spot->row == 8 && spot->dir == BOARD_HORIZONTAL_DIRECTION &&
        spot->num_tiles == 7) {
      underlaps_vac = true;
    }
  }
  assert(hooks_vac);
  assert(underlaps_vac);

  move_list_destroy(move_list);
  game_destroy(game);
  config_destroy(config);
}

void test_oxyphenbutazone_word_spot(void) {
  Config *config = config_create_or_die("set -lex CSW21 -wmp true");
  Game *game = config_game_create(config);
  Player *player = game_get_player(game, 0);
  player_set_move_sort_type(player, MOVE_SORT_SCORE);
  MoveList *move_list = move_list_create(10);

  WordSpotHeap spot_list;
  load_and_build_spots(game, VS_OXY, "OXPBAZE", &spot_list, move_list);
  const WordSpot *oxyphenbutazone_spot = &spot_list.spots[0];
  // Shadow is able to reduce this upper bound using hook restrictions (all the
  // way down to 1780). We don't do that for WordSpots but we can add it if it
  // helps.
  assert_spot_equity_int(oxyphenbutazone_spot, 1924);
  assert(oxyphenbutazone_spot->row == 0);
  assert(oxyphenbutazone_spot->col == 0);
  assert(oxyphenbutazone_spot->dir == BOARD_VERTICAL_DIRECTION);
  assert(oxyphenbutazone_spot->num_tiles == 7);

  move_list_destroy(move_list);
  game_destroy(game);
  config_destroy(config);
}

void test_word_spot(void) {
  test_zerk();
  // test_opening_racks();
  // test_best_leaves();
  // test_bingos_after_vac();
  // test_oxyphenbutazone_word_spot();
}