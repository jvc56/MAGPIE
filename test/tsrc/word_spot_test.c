#include <assert.h>

#include "../../src/impl/move_gen.h"

#include "../../src/impl/cgp.h"
#include "../../src/impl/config.h"

#include "test_constants.h"
#include "test_util.h"

void load_and_build_spots(Game *game, Player *player, const char *cgp,
                          const char *rack, WordSpotHeap *sorted_spots) {
  const LetterDistribution *ld = game_get_ld(game);
  Rack *player_rack = player_get_rack(player);

  game_load_cgp(game, cgp);
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

void test_opening_racks(void) {
  Config *config = config_create_or_die("set -lex CSW21 -wmp true");
  Game *game = config_game_create(config);
  Player *player = game_get_player(game, 0);

  WordSpotHeap spot_list;
  load_and_build_spots(game, player, EMPTY_CGP, "MUZJIKS", &spot_list);
  int expected_count = 7 + 6 + 5 + 4 + 3 + 2;
  assert(spot_list.count == expected_count);

  game_destroy(game);
  config_destroy(config);
}

void test_bingos_after_vac(void) {}

void test_oxyphenbutazone_word_spot(void) {}

void test_word_spot(void) {
  test_opening_racks();
  test_bingos_after_vac();
  test_oxyphenbutazone_word_spot();
}