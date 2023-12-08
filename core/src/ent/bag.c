#include <stdio.h>
#include <stdlib.h>

#include "../util/log.h"
#include "../util/util.h"

#include "bag.h"
#include "rack.h"
#include "xoshiro.h"

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

int get_tiles_remaining(const Bag *bag) {
  return bag->end_tile_index - bag->start_tile_index;
}

void shuffle(Bag *bag) {
  int tiles_remaining = get_tiles_remaining(bag);
  if (tiles_remaining > 1) {
    int i;
    for (i = bag->start_tile_index; i < bag->end_tile_index - 1; i++) {
      int j = i + xoshiro_get_random_number(bag->prng, bag->end_tile_index - i);
      int t = bag->tiles[j];
      bag->tiles[j] = bag->tiles[i];
      bag->tiles[i] = t;
    }
  }
}

void reset_bag(const LetterDistribution *letter_distribution, Bag *bag) {
  int tile_index = 0;
  int letter_distribution_size =
      letter_distribution_get_size(letter_distribution);
  for (int i = 0; i < letter_distribution_size; i++) {
    for (int k = 0;
         k < letter_distribution_get_distribution(letter_distribution, i);
         k++) {
      bag->tiles[tile_index] = i;
      tile_index++;
    }
  }
  bag->start_tile_index = 0;
  bag->end_tile_index = tile_index;
  shuffle(bag);
}

void update_bag(const LetterDistribution *letter_distribution, Bag *bag) {
  int total_tiles = letter_distribution_get_total_tiles(letter_distribution);
  if (bag->size != total_tiles) {
    free(bag->tiles);
    bag->size = total_tiles;
    bag->tiles = malloc_or_die(sizeof(uint8_t) * bag->size);
    reset_bag(letter_distribution, bag);
  }
}

Bag *create_bag(const LetterDistribution *letter_distribution) {
  Bag *bag = malloc_or_die(sizeof(Bag));
  // call reseed_prng if needed.
  bag->prng = create_prng(42);
  bag->size = letter_distribution_get_total_tiles(letter_distribution);
  bag->tiles = malloc_or_die(sizeof(uint8_t) * bag->size);
  reset_bag(letter_distribution, bag);
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
  new_bag->prng = create_prng(42);
  new_bag->size = bag->size;
  new_bag->tiles = malloc_or_die(sizeof(uint8_t) * new_bag->size);
  bag_copy(new_bag, bag);
  return new_bag;
}

void destroy_bag(Bag *bag) {
  destroy_prng(bag->prng);
  free(bag->tiles);
  free(bag);
}

bool bag_is_empty(const Bag *bag) { return get_tiles_remaining(bag) == 0; }

// This assumes the bag is shuffled and nonempty.
// The player index is used to determine which side
// of the bag the player draws from.
uint8_t draw_random_letter(Bag *bag, int player_draw_index) {
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

void draw_letter(Bag *bag, uint8_t letter, int player_draw_index) {
  if (is_blanked(letter)) {
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

void add_letter(Bag *bag, uint8_t letter, int player_draw_index) {
  if (is_blanked(letter)) {
    letter = BLANK_MACHINE_LETTER;
  }
  // Use (1 - player_draw_index) to shift 1 to the right when
  // adding a tile for player 0, since the added tile
  // needs to be added to the end of the bag.
  int insert_index = bag->start_tile_index - 1 + (1 - player_draw_index);
  int number_of_tiles_remaining = get_tiles_remaining(bag);
  if (number_of_tiles_remaining > 0) {
    insert_index +=
        xoshiro_get_random_number(bag->prng, number_of_tiles_remaining + 1);
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

void add_bag_to_rack(Bag *bag, Rack *rack) {
  for (int i = bag->start_tile_index; i < bag->end_tile_index; i++) {
    add_letter_to_rack(rack, bag->tiles[i]);
  }
}

void draw_at_most_to_rack(Bag *bag, Rack *rack, int n, int player_draw_index) {
  while (n > 0 && !bag_is_empty(bag)) {
    add_letter_to_rack(rack, draw_random_letter(bag, player_draw_index));
    n--;
  }
}