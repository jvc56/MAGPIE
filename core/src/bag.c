#include <stdlib.h>

#include "bag.h"
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