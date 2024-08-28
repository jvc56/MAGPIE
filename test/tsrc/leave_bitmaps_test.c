#include <assert.h>

#include "../../src/ent/leave_bitmaps.h"
#include "../../src/ent/letter_distribution.h"

#include "../../src/impl/config.h"
#include "../../src/impl/gameplay.h"

#include "test_util.h"

void test_leave_bitmaps(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  const LetterDistribution *ld = config_get_ld(config);

  Game *game = config_game_create(config);
  Rack *rack = player_get_rack(game_get_player(game, 0));
  Rack *empty_rack = rack_duplicate(rack);
  Bag *bag = game_get_bag(game);

  Rack *expected_rack = rack_create(ld_get_size(ld));
  int number_of_bitmaps = 6;
  LeaveBitMaps *lb = leave_bitmaps_create(ld, number_of_bitmaps);

  const int player_draw_index = 0;

  rack_set_to_string(ld, rack, "ABC");
  leave_bitmaps_set_leave(lb, rack, 0);

  rack_set_to_string(ld, rack, "BCD");
  leave_bitmaps_set_leave(lb, rack, 1);

  rack_set_to_string(ld, rack, "CDE");
  leave_bitmaps_set_leave(lb, rack, 2);

  rack_set_to_string(ld, rack, "VVYY");
  leave_bitmaps_set_leave(lb, rack, 3);

  rack_set_to_string(ld, rack, "FGGHII");
  leave_bitmaps_set_leave(lb, rack, 4);

  rack_set_to_string(ld, rack, "WWXYYZ");
  leave_bitmaps_set_leave(lb, rack, 5);

  rack_reset(rack);

  clear_bag(bag);

  bag_add_letter(bag, ld_hl_to_ml(ld, "A"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "C"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "D"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "E"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "X"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "Z"), 0);

  assert(leave_bitmaps_draw_first_available_subrack(
      lb, bag, rack, NULL, player_draw_index, number_of_bitmaps));

  rack_set_to_string(ld, expected_rack, "CDE");
  assert(racks_are_equal(expected_rack, rack));
  return_rack_to_bag(game, 0);

  bag_add_letter(bag, ld_hl_to_ml(ld, "B"), 0);

  assert(leave_bitmaps_draw_first_available_subrack(
      lb, bag, rack, NULL, player_draw_index, number_of_bitmaps));

  rack_set_to_string(ld, expected_rack, "ABC");
  assert(racks_are_equal(expected_rack, rack));
  assert(bag_get_tiles(bag) == 4);
  return_rack_to_bag(game, 0);
  assert(bag_get_tiles(bag) == 7);

  leave_bitmaps_swap_leaves(lb, 0, 1);

  assert(leave_bitmaps_draw_first_available_subrack(
      lb, bag, rack, NULL, player_draw_index, number_of_bitmaps));

  rack_set_to_string(ld, expected_rack, "BCD");
  assert(racks_are_equal(expected_rack, rack));
  assert(bag_get_tiles(bag) == 4);
  return_rack_to_bag(game, 0);
  assert(bag_get_tiles(bag) == 7);

  bag_draw_letter(bag, ld_hl_to_ml(ld, "C"), 0);

  assert(!leave_bitmaps_draw_first_available_subrack(
      lb, bag, rack, NULL, player_draw_index, number_of_bitmaps));
  assert(bag_get_tiles(bag) == 6);

  bag_add_letter(bag, ld_hl_to_ml(ld, "V"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "V"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "Y"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "Y"), 0);

  assert(leave_bitmaps_draw_first_available_subrack(
      lb, bag, rack, NULL, player_draw_index, number_of_bitmaps));

  rack_set_to_string(ld, expected_rack, "VVYY");
  assert(racks_are_equal(expected_rack, rack));
  assert(bag_get_tiles(bag) == 6);
  return_rack_to_bag(game, 0);
  assert(bag_get_tiles(bag) == 10);

  clear_bag(bag);
  assert(bag_get_tiles(bag) == 0);

  bag_add_letter(bag, ld_hl_to_ml(ld, "F"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "F"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "G"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "G"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "G"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "H"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "H"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "I"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "I"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "J"), 0);
  assert(bag_get_tiles(bag) == 10);

  // Relevent available leave: FGGHII

  rack_set_to_string(ld, rack, "FGHJK");

  // The FGGHIIJK is available in the pool but is more than 7
  // letters so it is not a valid leave.
  assert(!leave_bitmaps_draw_first_available_subrack(
      lb, bag, rack, NULL, player_draw_index, number_of_bitmaps));
  assert(bag_get_tiles(bag) == 10);

  // Use the rack to add another leave that will be available.
  rack_set_to_string(ld, rack, "FGGHIJ");
  leave_bitmaps_set_leave(lb, rack, 5);

  // Set the rack back to the original value to test it again.
  rack_set_to_string(ld, rack, "FGHJK");

  // The FGGHIJK is available in the pool and is exactly 7 letters so it should
  // be available.
  rack_reset(empty_rack);
  assert(leave_bitmaps_draw_first_available_subrack(
      lb, bag, rack, empty_rack, player_draw_index, number_of_bitmaps));
  assert(bag_get_tiles(bag) == 8);
  rack_set_to_string(ld, expected_rack, "FGGHIJK");
  assert(racks_are_equal(expected_rack, rack));
  rack_set_to_string(ld, expected_rack, "FGGHIJ");
  assert(racks_are_equal(expected_rack, empty_rack));

  // Test the max count argument
  clear_bag(bag);
  rack_reset(rack);

  bag_add_letter(bag, ld_hl_to_ml(ld, "A"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "C"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "D"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "E"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "X"), 0);
  bag_add_letter(bag, ld_hl_to_ml(ld, "Z"), 0);

  assert(!leave_bitmaps_draw_first_available_subrack(lb, bag, rack, NULL,
                                                     player_draw_index, 1));
  assert(bag_get_tiles(bag) == 6);

  assert(leave_bitmaps_draw_first_available_subrack(lb, bag, rack, NULL,
                                                    player_draw_index, 3));
  rack_set_to_string(ld, expected_rack, "CDE");
  assert(racks_are_equal(expected_rack, rack));
  assert(bag_get_tiles(bag) == 3);

  rack_destroy(expected_rack);
  rack_destroy(empty_rack);
  leave_bitmaps_destroy(lb);
  game_destroy(game);
  config_destroy(config);
}
