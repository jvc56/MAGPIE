#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "../src/config.h"
#include "../src/gameplay.h"
#include "../src/letter_distribution.h"
#include "../src/string_util.h"

#include "test_util.h"
#include "testconfig.h"

#define TEST_BAG_SIZE 100

void test_add_letter(const Config *config, Bag *bag, char *r,
                     char *expected_bag_string, int player_index) {
  add_letter(
      bag,
      human_readable_letter_to_machine_letter(config->letter_distribution, r),
      player_index);
  StringBuilder *bag_string = create_string_builder();
  string_builder_add_bag(bag, config->letter_distribution, 0, bag_string);
  assert_strings_equal(string_builder_peek(bag_string), expected_bag_string);
  destroy_string_builder(bag_string);
}

int get_drawn_tile_index(int drawn_tiles, int player_index) {
  return player_index * TEST_BAG_SIZE + drawn_tiles;
}

void test_bag(TestConfig *testconfig) {
  const Config *config = get_nwl_config(testconfig);
  Bag *bag = create_bag(config->letter_distribution);
  Rack *rack = create_rack(config->letter_distribution->size);

  int number_of_remaining_tiles = get_tiles_remaining(bag);
  for (int k = 0; k < number_of_remaining_tiles; k++) {
    uint8_t letter = draw_random_letter(bag, 0);
    add_letter_to_rack(rack, letter);
  }

  for (uint32_t i = 0; i < config->letter_distribution->size; i++) {
    assert((int)config->letter_distribution->distribution[i] == rack->array[i]);
  }

  reset_bag(bag, config->letter_distribution);
  reset_rack(rack);

  // Check drawing from the bag
  while (get_tiles_remaining(bag) > RACK_SIZE) {
    draw_at_most_to_rack(bag, rack, RACK_SIZE, 0);
    number_of_remaining_tiles -= RACK_SIZE;
    assert(!rack->empty);
    assert(rack->number_of_letters == RACK_SIZE);
    reset_rack(rack);
  }

  draw_at_most_to_rack(bag, rack, RACK_SIZE, 0);
  assert(bag_is_empty(bag));
  assert(!rack->empty);
  assert(rack->number_of_letters == number_of_remaining_tiles);
  reset_rack(rack);

  // Check adding letters to the bag

  test_add_letter(config, bag, "A", "A", 0);
  test_add_letter(config, bag, "F", "AF", 0);
  test_add_letter(config, bag, "Z", "AFZ", 0);
  test_add_letter(config, bag, "B", "ABFZ", 0);
  test_add_letter(config, bag, "a", "ABFZ?", 0);
  test_add_letter(config, bag, "b", "ABFZ??", 0);
  test_add_letter(config, bag, "z", "ABFZ???", 0);

  reset_bag(bag, config->letter_distribution);
  reset_rack(rack);

  Bag *bag_copy = copy_bag(bag);

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
  copy_bag_into(bag, bag_copy);
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

  copy_bag_into(bag, bag_copy);
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

  assert_bags_are_equal(bag, bag_copy, config->letter_distribution->size);

  copy_bag_into(bag, bag_copy);
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

  copy_bag_into(bag, bag_copy);
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

  assert_bags_are_equal(bag, bag_copy, config->letter_distribution->size);

  destroy_bag(bag);
  destroy_bag(bag_copy);
  destroy_rack(rack);
}
