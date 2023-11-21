#include <stdlib.h>

#include "bag.h"
#include "string_util.h"
#include "util.h"
#include "xoshiro.h"

#define BLANK_SORT_VALUE 255
struct Bag {
  int size;
  uint8_t *tiles;
  int start_tile_index;
  int end_tile_index;
  XoshiroPRNG *prng;
};

int get_tiles_remaining(const Bag *bag) {
  return (bag->end_tile_index - bag->start_tile_index) + 1;
}

void shuffle(Bag *bag) {
  int tiles_remaining = get_tiles_remaining(bag);
  if (tiles_remaining > 1) {
    int i;
    for (i = bag->start_tile_index; i < bag->end_tile_index; i++) {
      int j = i + xoshiro_next(bag->prng) /
                      (XOSHIRO_MAX / (bag->end_tile_index + 1 - i) + 1);
      int t = bag->tiles[j];
      bag->tiles[j] = bag->tiles[i];
      bag->tiles[i] = t;
    }
  }
}

void reset_bag(Bag *bag, const LetterDistribution *letter_distribution) {
  int tile_index = 0;
  for (uint32_t i = 0; i < (letter_distribution->size); i++) {
    for (uint32_t k = 0; k < letter_distribution->distribution[i]; k++) {
      bag->tiles[tile_index] = i;
      tile_index++;
    }
  }
  bag->start_tile_index = 0;
  bag->end_tile_index = tile_index - 1;
  shuffle(bag);
}

void update_bag(Bag *bag, const LetterDistribution *letter_distribution) {
  if (bag->size != letter_distribution->total_tiles) {
    free(bag->tiles);
    bag->size = letter_distribution->total_tiles;
    bag->tiles = malloc_or_die(sizeof(uint8_t) * bag->size);
    reset_bag(bag, letter_distribution);
  }
}

Bag *create_bag(const LetterDistribution *letter_distribution) {
  Bag *bag = malloc_or_die(sizeof(Bag));
  // call reseed_prng if needed.
  bag->prng = create_prng(42);
  bag->size = letter_distribution->total_tiles;
  bag->tiles = malloc_or_die(sizeof(uint8_t) * bag->size);
  reset_bag(bag, letter_distribution);
  return bag;
}

void copy_bag_into(Bag *dst, const Bag *src) {
  for (int tile_index = 0; tile_index <= src->end_tile_index; tile_index++) {
    dst->tiles[tile_index] = src->tiles[tile_index];
  }
  dst->end_tile_index = src->end_tile_index;
  copy_prng_into(dst->prng, src->prng);
}

Bag *copy_bag(const Bag *bag) {
  Bag *new_bag = malloc_or_die(sizeof(Bag));
  new_bag->prng = create_prng(42);
  new_bag->size = bag->size;
  new_bag->tiles = malloc_or_die(sizeof(uint8_t) * new_bag->size);
  copy_bag_into(new_bag, bag);
  return new_bag;
}

void destroy_bag(Bag *bag) {
  destroy_prng(bag->prng);
  free(bag->tiles);
  free(bag);
}

bool bag_is_empty(const Bag *bag) { return get_tiles_remaining(bag) == 0; }

// This assumes the bag is shuffled and nonempty
uint8_t draw_random_letter(Bag *bag, int player_index) {
  uint8_t letter = bag->tiles[bag->end_tile_index];
  // This assumes player_index can only be 0 or 1
  // Player 0 draws from the end of the bag and
  // player 1 draws from the start of the bag
  if (player_index == 0) {
    letter = bag->tiles[bag->end_tile_index];
    bag->end_tile_index--;
  } else {
    letter = bag->tiles[bag->start_tile_index];
    bag->start_tile_index++;
  }
  return letter;
}

// This assumes the letter is in the bag
// This function acts as if player 0 drew
// a tile and ignored it.
void remove_letter(Bag *bag, uint8_t letter) {
  if (is_blanked(letter)) {
    letter = BLANK_MACHINE_LETTER;
  }
  for (int i = bag->start_tile_index; i <= bag->end_tile_index; i++) {
    if (bag->tiles[i] == letter) {
      bag->tiles[i] = bag->tiles[bag->end_tile_index];
      bag->end_tile_index--;
      return;
    }
  }
}

void add_letter(Bag *bag, uint8_t letter) {
  if (is_blanked(letter)) {
    letter = BLANK_MACHINE_LETTER;
  }
  int insert_index = bag->start_tile_index;
  if (bag->end_tile_index > insert_index) {
    // XXX: should use division instead?
    insert_index += xoshiro_next(bag->prng) % get_tiles_remaining(bag);
  }
  bag->tiles[bag->end_tile_index + 1] = bag->tiles[insert_index];
  bag->tiles[insert_index] = letter;
  bag->end_tile_index++;
}

void reseed_prng(Bag *bag, uint64_t seed) { seed_prng(bag->prng, seed); }

// This function ensures that all workers for a given
// job are seeded with unique non-overlapping sequences
// for the PRNGs in their bags.
void seed_bag_for_worker(Bag *bag, uint64_t seed, int worker_index) {
  seed_prng(bag->prng, seed);
  for (int j = 0; j < worker_index; j++) {
    xoshiro_jump(bag->prng);
  }
}

void add_bag_to_rack(const Bag *bag, Rack *rack) {
  for (int i = 0; i <= bag->end_tile_index; i++) {
    add_letter_to_rack(rack, bag->tiles[i]);
  }
}

void string_builder_add_bag(const Bag *bag,
                            const LetterDistribution *letter_distribution,
                            size_t len, StringBuilder *bag_string_builder) {
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
  while (i < bag->end_tile_index + 1) {
    x = sorted_bag[i];
    k = i - 1;
    while (k >= 0 && x < sorted_bag[k]) {
      sorted_bag[k + 1] = sorted_bag[k];
      k--;
    }
    sorted_bag[k + 1] = x;
    i++;
  }

  for (int i = 0; i <= number_of_tiles_remaining; i++) {
    if (sorted_bag[i] == BLANK_SORT_VALUE) {
      sorted_bag[i] = 0;
    }
    string_builder_add_user_visible_letter(letter_distribution, sorted_bag[i],
                                           len, bag_string_builder);
  }
  free(sorted_bag);
}