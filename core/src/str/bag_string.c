#include "../ent/bag.h"
#include "../ent/letter_distribution.h"

#include "string_util.h"

#define BLANK_SORT_VALUE 255

void string_builder_add_bag(const Bag *bag,
                            const LetterDistribution *letter_distribution,
                            StringBuilder *bag_string_builder) {
  int number_of_tiles_remaining = get_tiles_remaining(bag);
  uint8_t *sorted_bag = malloc_or_die(sizeof(uint8_t) * bag->size);
  for (int i = 0; i < number_of_tiles_remaining; i++) {
    sorted_bag[i] = bag->tiles[i + bag->start_tile_index];
    // Make blanks some arbitrarily large number
    // so that they are printed last.
    if (sorted_bag[i] == BLANK_MACHINE_LETTER) {
      sorted_bag[i] = BLANK_SORT_VALUE;
    }
  }
  int x;
  int i = 1;
  int k;
  while (i < number_of_tiles_remaining) {
    x = sorted_bag[i];
    k = i - 1;
    while (k >= 0 && x < sorted_bag[k]) {
      sorted_bag[k + 1] = sorted_bag[k];
      k--;
    }
    sorted_bag[k + 1] = x;
    i++;
  }

  for (int i = 0; i < number_of_tiles_remaining; i++) {
    if (sorted_bag[i] == BLANK_SORT_VALUE) {
      sorted_bag[i] = 0;
    }
    string_builder_add_user_visible_letter(letter_distribution,
                                           bag_string_builder, sorted_bag[i]);
  }
  free(sorted_bag);
}