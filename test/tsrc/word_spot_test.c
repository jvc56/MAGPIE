#include <assert.h>

#include "../../src/impl/move_gen.h"

#include "../../src/impl/cgp.h"
#include "../../src/impl/config.h"
#include "../../src/impl/gameplay.h"

#include "test_constants.h"
#include "test_util.h"

void build_spots_for_current_position(Game *game, const char *rack,
                                      WordSpotHeap *sorted_spots) {
  const LetterDistribution *ld = game_get_ld(game);
  const int player_on_turn_idx = game_get_player_on_turn_index(game);
  Player *player = game_get_player(game, player_on_turn_idx);
  Rack *player_rack = player_get_rack(player);
  rack_set_to_string(ld, player_rack, rack);

  generate_spots_for_test(game);
  extract_sorted_spots_for_test(sorted_spots);
  Equity previous_equity = EQUITY_MAX_VALUE;
  const int number_of_spots = sorted_spots->count;
  for (int i = 0; i < number_of_spots; i++) {
    const Equity equity = sorted_spots->spots[i].best_possible_equity;
    assert(equity <= previous_equity);
    previous_equity = equity;
  }
}

void load_and_build_spots(Game *game, const char *cgp, const char *rack,
                          WordSpotHeap *sorted_spots) {

  game_load_cgp(game, cgp);
  build_spots_for_current_position(game, rack, sorted_spots);
}

void test_opening_racks(void) {
  Config *config = config_create_or_die("set -lex CSW21 -wmp true");
  Game *game = config_game_create(config);
  Player *player = game_get_player(game, 0);
  player_set_move_sort_type(player, MOVE_SORT_SCORE);

  WordSpotHeap spot_list;
  load_and_build_spots(game, EMPTY_CGP, "MUZJIKS", &spot_list);
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

  load_and_build_spots(game, EMPTY_CGP, "TRONGLE", &spot_list);
  expected_count = 6 + 5 + 4 + 3 + 2; // 7-letter word is not possible
  assert(spot_list.count == expected_count);
  const WordSpot *top_trongle_spot = &spot_list.spots[0];
  // We know there are sixes, and assume something could put the G on
  // the DWS even though none do.
  assert_spot_equity_int(top_trongle_spot, 18);

  load_and_build_spots(game, EMPTY_CGP, "VVWWXYZ", &spot_list);
  // WordSpot logic handles one-tile plays differently than shadow. We do not
  // record a spot for bogus one-tile words. One tile plays only form spots when
  // playing through tiles (and only a mainword direction).
  assert(spot_list.count == 0);

  game_destroy(game);
  config_destroy(config);
}

Equity get_leave_value(const Game *game, const char *leave) {
  const LetterDistribution *ld = game_get_ld(game);
  Rack *rack = rack_create(ld_get_size(ld));
  rack_set_to_string(ld, rack, leave);
  const int player_on_turn_idx = game_get_player_on_turn_index(game);
  const Player *player = game_get_player(game, player_on_turn_idx);
  const KLV *klv = player_get_klv(player);
  const Equity value = klv_get_leave_value(klv, rack);
  rack_destroy(rack);
  return value;
}

void test_best_leaves(void) {
  Config *config = config_create_or_die("set -lex CSW21 -wmp true");
  Game *game = config_game_create(config);
  Player *player = game_get_player(game, 0);
  player_set_move_sort_type(player, MOVE_SORT_EQUITY);

  WordSpotHeap spot_list;
  load_and_build_spots(game, EMPTY_CGP, "QUACKY?", &spot_list);
  int expected_count = 6 + 5 + 4 + 3 + 2;
  assert(spot_list.count == expected_count);
  const WordSpot *top_quacky_spot = &spot_list.spots[0];
  const Equity quacky_score = int_to_equity(68);
  const Equity blank_leave_value = get_leave_value(game, "?");
  assert(top_quacky_spot->num_tiles == 6);
  assert(top_quacky_spot->best_possible_equity ==
         quacky_score + blank_leave_value);

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
  player_set_move_sort_type(player, MOVE_SORT_EQUITY);

  WordSpotHeap spot_list;
  // Player 1: NWL
  load_and_build_spots(game, vac, "ORATES?", &spot_list);
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
  build_spots_for_current_position(game, "ORATES?", &spot_list);
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

  game_destroy(game);
  config_destroy(config);
}

void test_oxyphenbutazone_word_spot(void) {
  Config *config = config_create_or_die("set -lex CSW21 -wmp true");
  Game *game = config_game_create(config);
  Player *player = game_get_player(game, 0);
  player_set_move_sort_type(player, MOVE_SORT_SCORE);

  WordSpotHeap spot_list;
  load_and_build_spots(game, VS_OXY, "OXPBAZE", &spot_list);
  const WordSpot *oxyphenbutazone_spot = &spot_list.spots[0];
  // Shadow is able to reduce this upper bound using hooks restrictions (all the
  // way down to 1780). We don't do that for WordSpots but we can add it if it
  // helps.
  assert_spot_equity_int(oxyphenbutazone_spot, 1924);
  assert(oxyphenbutazone_spot->row == 0);
  assert(oxyphenbutazone_spot->col == 0);
  assert(oxyphenbutazone_spot->dir == BOARD_VERTICAL_DIRECTION);
  assert(oxyphenbutazone_spot->num_tiles == 7);

  game_destroy(game);
  config_destroy(config);
}

void test_word_spot(void) {
  test_opening_racks();
  test_best_leaves();
  test_bingos_after_vac();
  test_oxyphenbutazone_word_spot();
}