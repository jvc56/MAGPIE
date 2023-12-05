#include <stdint.h>

#include "../ent/bag.h"
#include "../ent/letter_distribution.h"
#include "../ent/rack.h"

#include "string_util.h"

#define BLANK_SORT_VALUE 255

void string_builder_add_bag(const Bag *bag,
                            const LetterDistribution *letter_distribution,
                            StringBuilder *bag_string_builder) {
  Bag *copied_bag = bag_duplicate(bag);
  uint32_t ld_size = letter_distribution_get_size(letter_distribution);
  Rack *bag_as_rack = create_rack(ld_size);

  int number_of_tiles = get_tiles_remaining(bag);
  for (int i = 0; i < number_of_tiles; i++) {
    add_letter_to_rack(bag_as_rack, draw_random_letter(copied_bag, 0));
  }

  for (int i = 1; i < ld_size; i++) {
    for (int j = 0; j < get_number_of_letter(bag_as_rack, i); j++) {
      string_builder_add_user_visible_letter(letter_distribution,
                                             bag_string_builder, i);
    }
  }

  // Print the blanks at the end
  for (int i = 0; i < get_number_of_letter(bag_as_rack, BLANK_MACHINE_LETTER);
       i++) {
    string_builder_add_user_visible_letter(
        letter_distribution, bag_string_builder, BLANK_MACHINE_LETTER);
  }
}