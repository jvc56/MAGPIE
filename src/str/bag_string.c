#include "../def/letter_distribution_defs.h"

#include "../ent/bag.h"
#include "../ent/letter_distribution.h"
#include "../ent/rack.h"

#include "bag_string.h"
#include "letter_distribution_string.h"

#include "../util/string_util.h"

enum { BLANK_SORT_VALUE = 255 };

void string_builder_add_bag(StringBuilder *bag_string_builder, const Bag *bag,
                            const LetterDistribution *ld) {
  int ld_size = ld_get_size(ld);
  Rack *bag_as_rack = rack_create(ld_size);

  Bag *copied_bag = bag_duplicate(bag);
  int number_of_tiles = bag_get_tiles(bag);
  for (int i = 0; i < number_of_tiles; i++) {
    rack_add_letter(bag_as_rack, bag_draw_random_letter(copied_bag, 0));
  }
  bag_destroy(copied_bag);

  for (int i = 1; i < ld_size; i++) {
    for (int j = 0; j < rack_get_letter(bag_as_rack, i); j++) {
      string_builder_add_user_visible_letter(bag_string_builder, ld, i);
    }
  }

  // Print the blanks at the end
  for (int i = 0; i < rack_get_letter(bag_as_rack, BLANK_MACHINE_LETTER); i++) {
    string_builder_add_user_visible_letter(bag_string_builder, ld,
                                           BLANK_MACHINE_LETTER);
  }
  rack_destroy(bag_as_rack);
}