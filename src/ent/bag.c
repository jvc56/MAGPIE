#include "bag.h"

#include <stdlib.h>

#include "../def/letter_distribution_defs.h"

#include "xoshiro.h"

#include "../util/log.h"
#include "../util/util.h"

struct Bag {
  int size;
  uint8_t *tiles;
  // Inclusive start index for when
  // tiles start in the 'tiles' array.
  int start_tile_index;
  // Exclusive end index for when
  // tiles end in the 'tiles' array.
  int end_tile_index;
  XoshiroPRNG *prng;
};

int bag_get_tiles(const Bag *bag) {
  return bag->end_tile_index - bag->start_tile_index;
}

void bag_shuffle(Bag *bag) {
  int tiles_remaining = bag_get_tiles(bag);
  if (tiles_remaining > 1) {
    int i;
    for (i = bag->start_tile_index; i < bag->end_tile_index - 1; i++) {
      int j = i + prng_get_random_number(bag->prng, bag->end_tile_index - i);
      int t = bag->tiles[j];
      bag->tiles[j] = bag->tiles[i];
      bag->tiles[i] = t;
    }
  }
}

void bag_reset(const LetterDistribution *ld, Bag *bag) {
  int tile_index = 0;
  int ld_size = ld_get_size(ld);
  for (int i = 0; i < ld_size; i++) {
    for (int k = 0; k < ld_get_dist(ld, i); k++) {
      bag->tiles[tile_index] = i;
      tile_index++;
    }
  }
  bag->start_tile_index = 0;
  bag->end_tile_index = tile_index;
  bag_shuffle(bag);
}

Bag *bag_create(const LetterDistribution *ld) {
  Bag *bag = malloc_or_die(sizeof(Bag));
  bag->prng = prng_create(42);
  bag->size = ld_get_total_tiles(ld);
  bag->tiles = malloc_or_die(sizeof(uint8_t) * bag->size);
  bag_reset(ld, bag);
  return bag;
}

void bag_copy(Bag *dst, const Bag *src) {
  for (int tile_index = 0; tile_index < src->end_tile_index; tile_index++) {
    dst->tiles[tile_index] = src->tiles[tile_index];
  }
  dst->start_tile_index = src->start_tile_index;
  dst->end_tile_index = src->end_tile_index;
  prng_copy(dst->prng, src->prng);
}

Bag *bag_duplicate(const Bag *bag) {
  Bag *new_bag = malloc_or_die(sizeof(Bag));
  new_bag->prng = prng_create(42);
  new_bag->size = bag->size;
  new_bag->tiles = malloc_or_die(sizeof(uint8_t) * new_bag->size);
  bag_copy(new_bag, bag);
  return new_bag;
}

void bag_destroy(Bag *bag) {
  if (!bag) {
    return;
  }
  prng_destroy(bag->prng);
  free(bag->tiles);
  free(bag);
}

bool bag_is_empty(const Bag *bag) { return bag_get_tiles(bag) == 0; }

// This assumes the bag is shuffled and nonempty.
// The player index is used to determine which side
// of the bag the player draws from.
uint8_t bag_draw_random_letter(Bag *bag, int player_draw_index) {
  uint8_t letter;
  // This assumes player_draw_index can only be 0 or 1
  // Player 0 draws from the end of the bag and
  // player 1 draws from the start of the bag
  if (player_draw_index == 0) {
    bag->end_tile_index--;
    letter = bag->tiles[bag->end_tile_index];
  } else {
    letter = bag->tiles[bag->start_tile_index];
    bag->start_tile_index++;
  }
  return letter;
}

void bag_draw_letter(Bag *bag, uint8_t letter, int player_draw_index) {
  if (get_is_blanked(letter)) {
    letter = BLANK_MACHINE_LETTER;
  }
  int letter_index = -1;
  for (int i = bag->start_tile_index; i < bag->end_tile_index; i++) {
    if (bag->tiles[i] == letter) {
      letter_index = i;
      break;
    }
  }
  if (letter_index < 0) {
    log_fatal("letter not found in bag: %d\n", letter);
  }
  if (player_draw_index == 0) {
    bag->end_tile_index--;
    bag->tiles[letter_index] = bag->tiles[bag->end_tile_index];
  } else {
    bag->tiles[letter_index] = bag->tiles[bag->start_tile_index];
    bag->start_tile_index++;
  }
}

void bag_add_letter(Bag *bag, uint8_t letter, int player_draw_index) {
  if (get_is_blanked(letter)) {
    letter = BLANK_MACHINE_LETTER;
  }
  // Use (1 - player_draw_index) to shift 1 to the right when
  // adding a tile for player 0, since the added tile
  // needs to be added to the end of the bag.
  int insert_index = bag->start_tile_index - 1 + (1 - player_draw_index);
  int number_of_tiles_remaining = bag_get_tiles(bag);
  if (number_of_tiles_remaining > 0) {
    insert_index +=
        prng_get_random_number(bag->prng, number_of_tiles_remaining + 1);
  }
  // Add swapped tiles
  // to the player's respective "side" of the bag.
  // Note: this assumes player index is only 0 or 1.
  if (player_draw_index == 0) {
    bag->tiles[bag->end_tile_index] = bag->tiles[insert_index];
    bag->end_tile_index++;
  } else {
    bag->tiles[bag->start_tile_index - 1] = bag->tiles[insert_index];
    bag->start_tile_index--;
  }
  bag->tiles[insert_index] = letter;
}

// This function ensures that all workers for a given
// job are seeded with unique non-overlapping sequences
// for the PRNGs in their bags.
void bag_seed_for_worker(Bag *bag, uint64_t seed, int worker_index) {
  prng_seed(bag->prng, seed);
  for (int j = 0; j < worker_index; j++) {
    prng_jump(bag->prng);
  }
}

// Gets the number of a tiles 'ml' in the bag. For drawing tiles
// to get the tile itself and update the bag, see
// the bag_draw* functions.
int bag_get_letter(const Bag *bag, uint8_t ml) {
  int sum = 0;
  for (int i = bag->start_tile_index; i < bag->end_tile_index; i++) {
    if (bag->tiles[i] == ml) {
      sum++;
    }
  }
  return sum;
}
