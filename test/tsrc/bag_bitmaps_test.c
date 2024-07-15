#include <assert.h>

#include "../../src/ent/bag_bitmaps.h"
#include "../../src/ent/letter_distribution.h"

#include "../../src/impl/config.h"

#include "test_util.h"

void test_bag_bitmaps(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  const LetterDistribution *ld = config_get_ld(config);
  Rack *expected_rack = rack_create(ld_get_size(ld));
  Rack *rack = rack_create(ld_get_size(ld));
  Bag *bag = bag_create(ld);
  BagBitMaps *bb = bag_bitmaps_create(ld, 10);

  rack_set_to_string(ld, rack, "ABC");
  bag_bitmaps_set_rack(bb, rack, 0);

  rack_set_to_string(ld, rack, "BCD");
  bag_bitmaps_set_rack(bb, rack, 1);

  rack_set_to_string(ld, rack, "CDE");
  bag_bitmaps_set_rack(bb, rack, 2);

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

  assert(bag_bitmaps_get_first_subrack(bb, bag, rack));

  rack_set_to_string(ld, expected_rack, "CDE");
  assert(racks_are_equal(expected_rack, rack));

  rack_destroy(rack);
  bag_destroy(bag);
  bag_bitmaps_destroy(bb);
  config_destroy(config);
}
