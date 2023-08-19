#include "bag_print.h"
#include "../src/bag.h"
#include "../src/letter_distribution.h"
#include "../src/util.h"

void write_bag_to_end_of_buffer(char *dest, Bag *bag,
                                LetterDistribution *letter_distribution) {
  // Must be lower than the max uint8_t value
  int blank_sort_value = 100;
  uint8_t sorted_bag[BAG_SIZE];
  for (int i = 0; i <= bag->last_tile_index; i++) {
    sorted_bag[i] = bag->tiles[i];
    // Make blanks some arbitrarily large number
    // so that they are printed last.
    if (sorted_bag[i] == 0) {
      sorted_bag[i] = blank_sort_value;
    }
  }
  int x;
  int i = 1;
  int k;
  while (i < bag->last_tile_index + 1) {
    x = sorted_bag[i];
    k = i - 1;
    while (k >= 0 && x < sorted_bag[k]) {
      sorted_bag[k + 1] = sorted_bag[k];
      k--;
    }
    sorted_bag[k + 1] = x;
    i++;
  }

  for (int i = 0; i <= bag->last_tile_index; i++) {
    if (sorted_bag[i] == blank_sort_value) {
      sorted_bag[i] = 0;
    }
    write_user_visible_letter_to_end_of_buffer(dest, letter_distribution,
                                               sorted_bag[i]);
  }
}