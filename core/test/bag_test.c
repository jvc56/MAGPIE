#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "../src/str/bag_string.h"
#include "../src/util/string_util.h"

#include "../src/impl/gameplay.h"

#include "../src/def/rack_defs.h"

#include "../src/ent/config.h"
#include "../src/ent/letter_distribution.h"

#include "test_util.h"
#include "testconfig.h"

#define TEST_BAG_SIZE 100

void test_add_letter(const Config *config, Bag *bag, char *r,
                     char *expected_bag_string, int player_index) {
  LetterDistribution *ld = config_get_letter_distribution(config);
  add_letter(bag, hl_to_ml(ld, r), player_index);
  StringBuilder *bag_string = create_string_builder();
  string_builder_add_bag(bag, ld, bag_string);
  assert_strings_equal(string_builder_peek(bag_string), expected_bag_string);
  destroy_string_builder(bag_string);
}

int get_drawn_tile_index(int drawn_tiles, int player_index) {
  return player_index * TEST_BAG_SIZE + drawn_tiles;
}

void test_bag(TestConfig *testconfig) {
  const Config *config = get_nwl_config(testconfig);
  const LetterDistribution *ld = config_get_letter_distribution(config);
  int ld_size = letter_distribution_get_size(ld);
  Bag *bag = create_bag(ld);
  Rack *rack = create_rack(ld_size);
  Rack *rack2 = create_rack(ld_size);

  int number_of_remaining_tiles = get_tiles_remaining(bag);
  for (int k = 0; k < number_of_remaining_tiles; k++) {
    uint8_t letter = draw_random_letter(bag, 0);
    add_letter_to_rack(rack, letter);
  }

  for (int i = 0; i < ld_size; i++) {
    assert((int)letter_distribution_get_distribution(ld, i) ==
           get_number_of_letter(rack, i));
  }

  reset_bag(ld, bag);
  reset_rack(rack);

  // Check drawing from the bag
  int drawing_player = 0;
  while (get_tiles_remaining(bag) > RACK_SIZE) {
    draw_at_most_to_rack(bag, rack, RACK_SIZE, drawing_player);
    drawing_player = 1 - drawing_player;
    number_of_remaining_tiles -= RACK_SIZE;
    assert(rack_is_empty(rack));
    assert(get_number_of_letters(rack) == RACK_SIZE);
    reset_rack(rack);
  }

  draw_at_most_to_rack(bag, rack, RACK_SIZE, drawing_player);
  assert(bag_is_empty(bag));
  assert(rack_is_empty(rack));
  assert(get_number_of_letters(rack) == number_of_remaining_tiles);
  reset_rack(rack);

  // Check adding letters to the bag

  test_add_letter(config, bag, "A", "A", 0);
  test_add_letter(config, bag, "F", "AF", 1);
  test_add_letter(config, bag, "Z", "AFZ", 0);
  test_add_letter(config, bag, "B", "ABFZ", 1);
  test_add_letter(config, bag, "a", "ABFZ?", 0);
  test_add_letter(config, bag, "b", "ABFZ??", 1);
  test_add_letter(config, bag, "z", "ABFZ???", 0);

  add_bag_to_rack(bag, rack);
  set_rack_to_string(ld, rack2, "ABFZ???");
  assert(racks_are_equal(rack, rack2));

  reset_bag(ld, bag);
  reset_rack(rack);

  Bag *copy_of_bag = bag_duplicate(bag);

  // The first (TEST_BAG_SIZE) / 2 tiles
  // are drawn by player 0 and the next (TEST_BAG_SIZE) / 2
  // tiles are drawn by player 1
  uint8_t draw_order[(TEST_BAG_SIZE) * 2];
  uint8_t tiles_drawn[2] = {0, 0};

  // Establish the initial draw order
  for (int i = 0; i < (TEST_BAG_SIZE); i++) {
    int player_index = i % 2;
    uint8_t letter = draw_random_letter(bag, player_index);
    int tiles_index =
        get_drawn_tile_index(tiles_drawn[player_index]++, player_index);
    draw_order[tiles_index] = letter;
  }
  assert(bag_is_empty(bag));

  // Ensure that any interleaving of drawn tiles
  // by player 0 and 1 results in the same order
  // for both players.
  bag_copy(bag, copy_of_bag);
  tiles_drawn[0] = 0;
  tiles_drawn[1] = 0;

  for (int i = 0; i < (TEST_BAG_SIZE); i++) {
    int player_index = i / ((TEST_BAG_SIZE) / 2);
    uint8_t letter = draw_random_letter(bag, player_index);
    int tiles_index =
        get_drawn_tile_index(tiles_drawn[player_index]++, player_index);
    assert(draw_order[tiles_index] == letter);
  }
  assert(bag_is_empty(bag));

  bag_copy(bag, copy_of_bag);
  tiles_drawn[0] = 0;
  tiles_drawn[1] = 0;

  for (int i = 0; i < (TEST_BAG_SIZE); i++) {
    int player_index = (i % 10) / 5;
    uint8_t letter = draw_random_letter(bag, player_index);
    int tiles_index =
        get_drawn_tile_index(tiles_drawn[player_index]++, player_index);
    assert(draw_order[tiles_index] == letter);
  }
  assert(bag_is_empty(bag));

  // Add the tiles back
  for (int i = 0; i < (TEST_BAG_SIZE); i++) {
    int player_index = (i % 10) / 5;
    tiles_drawn[player_index]--;
    int tiles_index =
        get_drawn_tile_index(tiles_drawn[player_index], player_index);
    add_letter(bag, draw_order[tiles_index], player_index);
  }

  assert_bags_are_equal(bag, copy_of_bag, ld_size);

  bag_copy(bag, copy_of_bag);
  tiles_drawn[0] = 0;
  tiles_drawn[1] = 0;

  // One player draws way more than the other
  for (int i = 0; i < (TEST_BAG_SIZE); i++) {
    int player_index = i / ((TEST_BAG_SIZE)-10);
    uint8_t letter = draw_random_letter(bag, player_index);
    int tiles_index;
    if (i < (TEST_BAG_SIZE) / 2) {
      // Draws from the first half of the bag
      // should match the established order
      tiles_index = get_drawn_tile_index(i, 0);
    } else if (i < (TEST_BAG_SIZE)-10) {
      // Now player 0 is drawing from player 1's "half"
      // of the bag. The effect is that player 0 draws
      // tiles in order starting from the last tile
      // drawn by player 1
      tiles_index = get_drawn_tile_index((TEST_BAG_SIZE)-1 - i, 1);
    } else {
      // Now player 1 draws their tiles
      tiles_index = get_drawn_tile_index(i - ((TEST_BAG_SIZE)-10), 1);
    }
    assert(draw_order[tiles_index] == letter);
  }
  assert(bag_is_empty(bag));

  bag_copy(bag, copy_of_bag);
  tiles_drawn[0] = 0;
  tiles_drawn[1] = 0;

  // Player 1 draws all tiles
  for (int i = 0; i < (TEST_BAG_SIZE); i++) {
    uint8_t letter = draw_random_letter(bag, 1);
    int tiles_index;
    if (i < (TEST_BAG_SIZE) / 2) {
      tiles_index = get_drawn_tile_index(i, 1);
    } else {
      tiles_index = get_drawn_tile_index((TEST_BAG_SIZE)-1 - i, 0);
    }
    assert(draw_order[tiles_index] == letter);
  }
  assert(bag_is_empty(bag));

  // Add the tiles back
  for (int i = 0; i < (TEST_BAG_SIZE); i++) {
    int tiles_index;
    if (i < (TEST_BAG_SIZE) / 2) {
      tiles_index = get_drawn_tile_index(i, 1);
    } else {
      tiles_index = get_drawn_tile_index((TEST_BAG_SIZE)-1 - i, 0);
    }
    add_letter(bag, draw_order[tiles_index], 1);
  }

  assert_bags_are_equal(bag, copy_of_bag, ld_size);

  destroy_bag(bag);
  destroy_bag(copy_of_bag);
  destroy_rack(rack);
  destroy_rack(rack2);
}
