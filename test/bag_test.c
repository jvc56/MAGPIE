#include "bag_test.h"

#include "../src/def/letter_distribution_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/rack.h"
#include "../src/impl/config.h"
#include "../src/str/bag_string.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdint.h>

enum { TEST_BAG_SIZE = 100, TEST_ROTATE_TILES = 10 };

void test_add_letter(const Config *config, Bag *bag, const char *r,
                     const char *expected_bag_string, const int player_index) {
  const LetterDistribution *ld = config_get_ld(config);
  bag_add_letter(bag, ld_hl_to_ml(ld, r), player_index);
  StringBuilder *bag_string = string_builder_create();
  string_builder_add_bag(bag_string, bag, ld);
  assert_strings_equal(string_builder_peek(bag_string), expected_bag_string);
  string_builder_destroy(bag_string);
}

int get_drawn_tile_index(int drawn_tiles, int player_index) {
  return player_index * TEST_BAG_SIZE + drawn_tiles;
}

// Asserts the bag holds tiles[(offset + i) % TEST_ROTATE_TILES] for its
// remaining letters, from its start.
static void assert_bag_rotated(const Bag *bag, const MachineLetter *tiles,
                               int offset, int first, int count) {
  MachineLetter peeked[TEST_ROTATE_TILES];
  assert(bag_peek_tiles(bag, peeked) == count);
  for (int tile_idx = 0; tile_idx < count; tile_idx++) {
    assert(peeked[tile_idx] == tiles[first + ((offset + tile_idx) % count)]);
  }
}

// bag_rotate moves the letter shift places from the start to the start and
// wraps the letters before it to the end, takes the shift modulo the letters
// left, and rotates only the letters still in a partly drawn bag.
static void test_bag_rotate(const LetterDistribution *ld) {
  Bag *bag = bag_create(ld, 0);
  MachineLetter tiles[TEST_ROTATE_TILES];
  for (int tile_idx = 0; tile_idx < TEST_ROTATE_TILES; tile_idx++) {
    tiles[tile_idx] = (MachineLetter)(tile_idx + 1);
  }
  bag_set_to_tiles(bag, tiles, TEST_ROTATE_TILES);
  bag_rotate(bag, 3);
  assert_bag_rotated(bag, tiles, 3, 0, TEST_ROTATE_TILES);
  // A full turn more, and a shift past the size, are taken modulo it.
  bag_rotate(bag, TEST_ROTATE_TILES + 4);
  assert_bag_rotated(bag, tiles, 7, 0, TEST_ROTATE_TILES);
  bag_rotate(bag, 3);
  assert_bag_rotated(bag, tiles, 0, 0, TEST_ROTATE_TILES);
  // Player 1 draws from the start and player 0 from the end, so after a
  // rotation by 2 they draw what were the third and the second letters.
  bag_rotate(bag, 2);
  assert(bag_draw_random_letter(bag, 1) == tiles[2]);
  assert(bag_draw_random_letter(bag, 0) == tiles[1]);
  // The 8 letters left, tiles[3..9] then tiles[0], rotate among themselves.
  MachineLetter left[TEST_ROTATE_TILES];
  const int num_left = bag_peek_tiles(bag, left);
  assert(num_left == TEST_ROTATE_TILES - 2);
  bag_rotate(bag, 5);
  MachineLetter rotated[TEST_ROTATE_TILES];
  assert(bag_peek_tiles(bag, rotated) == num_left);
  for (int tile_idx = 0; tile_idx < num_left; tile_idx++) {
    assert(rotated[tile_idx] == left[(tile_idx + 5) % num_left]);
  }
  bag_destroy(bag);
}

void test_bag(void) {
  Config *config = config_create_or_die(
      "set -lex NWL20 -s1 score -s2 score -r1 all -r2 all -numplays 1");
  const LetterDistribution *ld = config_get_ld(config);
  int ld_size = ld_get_size(ld);
  Bag *bag = bag_create(ld, 0);
  Rack *rack = rack_create(ld_size);

  for (int i = 0; i < ld_size; i++) {
    assert((int)ld_get_dist(ld, i) == bag_get_letter(bag, i));
  }

  int number_of_remaining_tiles = bag_get_letters(bag);
  for (int k = 0; k < number_of_remaining_tiles; k++) {
    MachineLetter letter = bag_draw_random_letter(bag, 0);
    rack_add_letter(rack, letter);
  }

  for (int i = 0; i < ld_size; i++) {
    assert((int)ld_get_dist(ld, i) == rack_get_letter(rack, i));
  }

  bag_reset(ld, bag);
  rack_reset(rack);

  while (!bag_is_empty(bag)) {
    bag_draw_random_letter(bag, bag_get_letters(bag) % 2);
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
  MachineLetter draw_order[(TEST_BAG_SIZE) * 2];
  MachineLetter tiles_drawn[2] = {0, 0};

  // Establish the initial draw order
  for (int i = 0; i < (TEST_BAG_SIZE); i++) {
    int player_index = i % 2;
    MachineLetter letter = bag_draw_random_letter(bag, player_index);
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
    MachineLetter letter = bag_draw_random_letter(bag, player_index);
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
    MachineLetter letter = bag_draw_random_letter(bag, player_index);
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

  assert_bags_are_equal(bag, copy_of_bag);

  bag_copy(bag, copy_of_bag);

  // One player draws way more than the other
  for (int i = 0; i < (TEST_BAG_SIZE); i++) {
    int player_index = i / ((TEST_BAG_SIZE)-10);
    MachineLetter letter = bag_draw_random_letter(bag, player_index);
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

  // Player 1 draws all tiles
  for (int i = 0; i < (TEST_BAG_SIZE); i++) {
    MachineLetter letter = bag_draw_random_letter(bag, 1);
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

  assert_bags_are_equal(bag, copy_of_bag);

  test_bag_rotate(ld);

  bag_destroy(bag);
  bag_destroy(copy_of_bag);
  rack_destroy(rack);
  config_destroy(config);
}
