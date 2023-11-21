#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "../src/config.h"
#include "../src/gameplay.h"
#include "../src/letter_distribution.h"
#include "../src/string_util.h"

#include "test_util.h"
#include "testconfig.h"

void test_add_letter(const Config *config, Bag *bag, char *r,
                     char *expected_bag_string) {
  add_letter(bag, human_readable_letter_to_machine_letter(
                      config->letter_distribution, r));
  StringBuilder *bag_string = create_string_builder();
  string_builder_add_bag(bag, config->letter_distribution, 0, bag_string);
  assert_strings_equal(string_builder_peek(bag_string), expected_bag_string);
  destroy_string_builder(bag_string);
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

  test_add_letter(config, bag, "A", "A");
  test_add_letter(config, bag, "F", "AF");
  test_add_letter(config, bag, "Z", "AFZ");
  test_add_letter(config, bag, "B", "ABFZ");
  test_add_letter(config, bag, "a", "ABFZ?");
  test_add_letter(config, bag, "b", "ABFZ??");
  test_add_letter(config, bag, "z", "ABFZ???");

  destroy_bag(bag);
  destroy_rack(rack);
}
