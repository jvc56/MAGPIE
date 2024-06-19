#include <assert.h>
#include <stdint.h>

#include "../../src/ent/bag.h"
#include "../../src/ent/letter_distribution.h"
#include "../../src/ent/rack.h"
#include "../../src/impl/config.h"

#include "../../src/str/bag_string.h"

#include "../../src/util/string_util.h"

#include "test_util.h"

#define TEST_BAG_SIZE 100

void test_add_letter(const Config *config, Bag *bag, char *r,
                     char *expected_bag_string, int player_index) {
  LetterDistribution *ld = config_get_ld(config);
  bag_add_letter(bag, ld_hl_to_ml(ld, r), player_index);
  StringBuilder *bag_string = create_string_builder();
  string_builder_add_bag(bag, ld, bag_string);
  assert_strings_equal(string_builder_peek(bag_string), expected_bag_string);
  destroy_string_builder(bag_string);
}

int get_drawn_tile_index(int drawn_tiles, int player_index) {
  return player_index * TEST_BAG_SIZE + drawn_tiles;
}

void test_bag() {
  Config *config = create_config_or_die(
      "set -lex NWL20 -s1 score -s2 score -r1 all -r2 all -numplays 1");
  const LetterDistribution *ld = config_get_ld(config);
  int ld_size = ld_get_size(ld);
  Bag *bag = bag_create(ld);
  Rack *rack = rack_create(ld_size);

  for (int i = 0; i < ld_size; i++) {
    assert((int)ld_get_dist(ld, i) == bag_get_letter(bag, i));
  }

  int number_of_remaining_tiles = bag_get_tiles(bag);
  for (int k = 0; k < number_of_remaining_tiles; k++) {
    uint8_t letter = bag_draw_random_letter(bag, 0);
    rack_add_letter(rack, letter);
  }

  for (int i = 0; i < ld_size; i++) {
    assert((int)ld_get_dist(ld, i) == rack_get_letter(rack, i));
  }

  bag_reset(ld, bag);
  rack_reset(rack);

  while (!bag_is_empty(bag)) {
    bag_draw_random_letter(bag, bag_get_tiles(bag) % 2);
  }

  // Check adding letters to the bag

  test_add_letter(config, bag, "A", "A", 0);
  test_add_letter(config, bag, "F", "AF", 1);
  test_add_letter(config, bag, "Z", "AFZ", 0);
  test_add_letter(config, bag, "B", "ABFZ", 1);
  test_add_letter(config, bag, "a", "ABFZ?", 0);
  test_add_letter(config, bag, "b", "ABFZ??", 1);
  test_add_letter(config, bag, "z", "ABFZ???", 0);

  bag_reset(ld, bag);
  rack_reset(rack);

  Bag *copy_of_bag = bag_duplicate(bag);

  // The first (TEST_BAG_SIZE) / 2 tiles
  // are drawn by player 0 and the next (TEST_BAG_SIZE) / 2
  // tiles are drawn by player 1
  uint8_t draw_order[(TEST_BAG_SIZE) * 2];
  uint8_t tiles_drawn[2] = {0, 0};

  // Establish the initial draw order
  for (int i = 0; i < (TEST_BAG_SIZE); i++) {
    int player_index = i % 2;
    uint8_t letter = bag_draw_random_letter(bag, player_index);
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
    uint8_t letter = bag_draw_random_letter(bag, player_index);
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
    uint8_t letter = bag_draw_random_letter(bag, player_index);
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
    bag_add_letter(bag, draw_order[tiles_index], player_index);
  }

  assert_bags_are_equal(bag, copy_of_bag, ld_size);

  bag_copy(bag, copy_of_bag);
  tiles_drawn[0] = 0;
  tiles_drawn[1] = 0;

  // One player draws way more than the other
  for (int i = 0; i < (TEST_BAG_SIZE); i++) {
    int player_index = i / ((TEST_BAG_SIZE)-10);
    uint8_t letter = bag_draw_random_letter(bag, player_index);
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
    uint8_t letter = bag_draw_random_letter(bag, 1);
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
    bag_add_letter(bag, draw_order[tiles_index], 1);
  }

  assert_bags_are_equal(bag, copy_of_bag, ld_size);

  bag_destroy(bag);
  bag_destroy(copy_of_bag);
  rack_destroy(rack);
  config_destroy(config);
}
