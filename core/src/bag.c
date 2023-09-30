#include <stdlib.h>

#include "bag.h"
#include "string_builder.h"
#include "util.h"
#include "xoshiro.h"

void shuffle(Bag *bag) {
  if (bag->last_tile_index > 0) {
    int i;
    for (i = 0; i < bag->last_tile_index; i++) {
      int j = i + xoshiro_next(bag->prng) /
                      (XOSHIRO_MAX / (bag->last_tile_index + 1 - i) + 1);
      int t = bag->tiles[j];
      bag->tiles[j] = bag->tiles[i];
      bag->tiles[i] = t;
    }
  }
}

void reset_bag(Bag *bag, LetterDistribution *letter_distribution) {
  int idx = 0;
  for (uint32_t i = 0; i < (letter_distribution->size); i++) {
    for (uint32_t k = 0; k < letter_distribution->distribution[i]; k++) {
      bag->tiles[idx] = i;
      idx++;
    }
  }
  bag->last_tile_index = sizeof(bag->tiles) - 1;
  shuffle(bag);
}

Bag *create_bag(LetterDistribution *letter_distribution) {
  Bag *bag = malloc(sizeof(Bag));
  // call reseed_prng if needed.
  bag->prng = create_prng(42);
  reset_bag(bag, letter_distribution);
  return bag;
}

Bag *copy_bag(Bag *bag) {
  Bag *new_bag = malloc(sizeof(Bag));
  new_bag->prng = create_prng(42);
  copy_bag_into(new_bag, bag);
  return new_bag;
}

void copy_bag_into(Bag *dst, Bag *src) {
  for (int tile_index = 0; tile_index <= src->last_tile_index; tile_index++) {
    dst->tiles[tile_index] = src->tiles[tile_index];
  }
  dst->last_tile_index = src->last_tile_index;
  copy_prng_into(dst->prng, src->prng);
}

void destroy_bag(Bag *bag) {
  destroy_prng(bag->prng);
  free(bag);
}

// This assumes the letter is in the bag
void draw_letter(Bag *bag, uint8_t letter) {
  if (is_blanked(letter)) {
    letter = BLANK_MACHINE_LETTER;
  }
  for (int i = 0; i <= bag->last_tile_index; i++) {
    if (bag->tiles[i] == letter) {
      bag->tiles[i] = bag->tiles[bag->last_tile_index];
      bag->last_tile_index--;
      return;
    }
  }
}

void add_letter(Bag *bag, uint8_t letter) {
  if (is_blanked(letter)) {
    letter = BLANK_MACHINE_LETTER;
  }
  int insert_index = 0;
  if (bag->last_tile_index >= 0) {
    // XXX: should use division instead?
    insert_index = xoshiro_next(bag->prng) % (bag->last_tile_index + 1);
  }
  bag->tiles[bag->last_tile_index + 1] = bag->tiles[insert_index];
  bag->tiles[insert_index] = letter;
  bag->last_tile_index++;
}

void reseed_prng(Bag *bag, uint64_t seed) { seed_prng(bag->prng, seed); }

void write_bag(char *dest, Bag *bag, LetterDistribution *letter_distribution) {
  // Must be lower than the max uint8_t value
  int blank_sort_value = 200;
  uint8_t sorted_bag[BAG_SIZE];
  for (int i = 0; i <= bag->last_tile_index; i++) {
    sorted_bag[i] = bag->tiles[i];
    // Make blanks some arbitrarily large number
    // so that they are printed last.
    if (sorted_bag[i] == BLANK_MACHINE_LETTER) {
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
    write_user_visible_letter(dest, letter_distribution, sorted_bag[i]);
  }
}

void string_builder_add_bag(Bag *bag, LetterDistribution *letter_distribution,
                            size_t len, StringBuilder *string_builder) {

  char bag_string[BAG_SIZE * MAX_LETTER_CHAR_LENGTH] = "";
  write_bag(bag_string, bag, letter_distribution);
  string_builder_add_string(string_builder, bag_string, len);
}