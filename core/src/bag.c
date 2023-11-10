#include <stdlib.h>

#include "bag.h"
#include "string_util.h"
#include "util.h"
#include "xoshiro.h"

#define BLANK_SORT_VALUE 255

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
  int tile_index = 0;
  for (uint32_t i = 0; i < (letter_distribution->size); i++) {
    for (uint32_t k = 0; k < letter_distribution->distribution[i]; k++) {
      bag->tiles[tile_index] = i;
      tile_index++;
    }
  }
  bag->last_tile_index = tile_index - 1;
  shuffle(bag);
}

void update_bag(Bag *bag, LetterDistribution *letter_distribution) {
  if (bag->size != letter_distribution->total_tiles) {
    free(bag->tiles);
    bag->size = letter_distribution->total_tiles;
    bag->tiles = malloc_or_die(sizeof(uint8_t) * bag->size);
    reset_bag(bag, letter_distribution);
  }
}

Bag *create_bag(LetterDistribution *letter_distribution) {
  Bag *bag = malloc_or_die(sizeof(Bag));
  // call reseed_prng if needed.
  bag->prng = create_prng(42);
  bag->size = letter_distribution->total_tiles;
  bag->tiles = malloc_or_die(sizeof(uint8_t) * bag->size);
  reset_bag(bag, letter_distribution);
  return bag;
}

Bag *copy_bag(Bag *bag) {
  Bag *new_bag = malloc_or_die(sizeof(Bag));
  new_bag->prng = create_prng(42);
  new_bag->size = bag->size;
  new_bag->tiles = malloc_or_die(sizeof(uint8_t) * new_bag->size);
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
  free(bag->tiles);
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

void string_builder_add_bag(Bag *bag, LetterDistribution *letter_distribution,
                            size_t len, StringBuilder *bag_string_builder) {
  uint8_t *sorted_bag = malloc_or_die(sizeof(uint8_t) * bag->size);
  for (int i = 0; i <= bag->last_tile_index; i++) {
    sorted_bag[i] = bag->tiles[i];
    // Make blanks some arbitrarily large number
    // so that they are printed last.
    if (sorted_bag[i] == BLANK_MACHINE_LETTER) {
      sorted_bag[i] = BLANK_SORT_VALUE;
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
    if (sorted_bag[i] == BLANK_SORT_VALUE) {
      sorted_bag[i] = 0;
    }
    string_builder_add_user_visible_letter(letter_distribution, sorted_bag[i],
                                           len, bag_string_builder);
  }
  free(sorted_bag);
}