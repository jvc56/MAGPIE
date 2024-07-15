#include <assert.h>

#include "../../src/ent/bag_bitmaps.h"
#include "../../src/ent/letter_distribution.h"

#include "../../src/impl/config.h"
#include "../../src/impl/gameplay.h"

#include "test_util.h"

void test_bag_bitmaps(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  const LetterDistribution *ld = config_get_ld(config);

  Game *game = config_game_create(config);
  Rack *rack = player_get_rack(game_get_player(game, 0));
  Bag *bag = game_get_bag(game);

  Rack *expected_rack = rack_create(ld_get_size(ld));
  BagBitMaps *bb = bag_bitmaps_create(ld, 4);

  const int player_draw_index = 0;

  rack_set_to_string(ld, rack, "ABC");
  bag_bitmaps_set_bitmap(bb, rack, 0);

  rack_set_to_string(ld, rack, "BCD");
  bag_bitmaps_set_bitmap(bb, rack, 1);

  rack_set_to_string(ld, rack, "CDE");
  bag_bitmaps_set_bitmap(bb, rack, 2);

  rack_set_to_string(ld, rack, "VVYY");
  bag_bitmaps_set_bitmap(bb, rack, 3);

  rack_reset(rack);

  // Clear the bag
  int number_of_letters = bag_get_tiles(bag);
  for (int i = 0; i < number_of_letters; i++) {
    bag_draw_random_letter(bag, 0);
  }

  bag_add_letter(bag, ld_hl_to_ml(ld, "A"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "C"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "D"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "E"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "X"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "Z"), 0);

  assert(bag_bitmaps_draw_first_available_subrack(bb, bag, rack,
                                                  player_draw_index));

  rack_set_to_string(ld, expected_rack, "CDE");
  assert(racks_are_equal(expected_rack, rack));
  return_rack_to_bag(game, 0);

  bag_add_letter(bag, ld_hl_to_ml(ld, "B"), 0);

  assert(bag_bitmaps_draw_first_available_subrack(bb, bag, rack,
                                                  player_draw_index));

  rack_set_to_string(ld, expected_rack, "ABC");
  assert(racks_are_equal(expected_rack, rack));
  assert(bag_get_tiles(bag) == 4);
  return_rack_to_bag(game, 0);
  assert(bag_get_tiles(bag) == 7);

  bag_bitmaps_swap(bb, 0, 1);

  assert(bag_bitmaps_draw_first_available_subrack(bb, bag, rack,
                                                  player_draw_index));

  rack_set_to_string(ld, expected_rack, "BCD");
  assert(racks_are_equal(expected_rack, rack));
  assert(bag_get_tiles(bag) == 4);
  return_rack_to_bag(game, 0);
  assert(bag_get_tiles(bag) == 7);

  bag_draw_letter(bag, ld_hl_to_ml(ld, "C"), 0);

  assert(!bag_bitmaps_draw_first_available_subrack(bb, bag, rack,
                                                   player_draw_index));
  assert(bag_get_tiles(bag) == 6);

  bag_add_letter(bag, ld_hl_to_ml(ld, "V"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "V"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "Y"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "Y"), 0);

  assert(bag_bitmaps_draw_first_available_subrack(bb, bag, rack,
                                                  player_draw_index));

  rack_set_to_string(ld, expected_rack, "VVYY");
  assert(racks_are_equal(expected_rack, rack));
  assert(bag_get_tiles(bag) == 6);
  return_rack_to_bag(game, 0);
  assert(bag_get_tiles(bag) == 10);

  rack_destroy(expected_rack);
  bag_bitmaps_destroy(bb);
  game_destroy(game);
  config_destroy(config);
}
