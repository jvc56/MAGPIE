#ifndef BAG_H
#define BAG_H

#include "letter_distribution.h"
#include "xoshiro.h"
#include <stdbool.h>
#include <stdint.h>

enum { MAX_BAG_SIZE = 1000 };

typedef struct Bag Bag;

struct Bag {
  // The total number of letters
  // in the bag for the initial state.
  int size;
  MachineLetter *letters;
  // Inclusive start index for when
  // letters start in the 'letters' array.
  int start_tile_index;
  // Exclusive end index for when
  // letters end in the 'letters' array.
  int end_tile_index;
  // Deterministic PRNG for the bag.
  XoshiroPRNG *prng;
};

Bag *bag_create(const LetterDistribution *ld, uint64_t seed);
void bag_destroy(Bag *bag);
void bag_copy(Bag *dst, const Bag *src);
Bag *bag_duplicate(const Bag *bag, const LetterDistribution *ld);

int bag_get_letters(const Bag *bag);
int bag_get_letter(const Bag *bag, MachineLetter ml);
bool bag_is_empty(const Bag *bag);

void bag_set_letter(Bag *bag, int index, MachineLetter ml);
void bag_set_start_tile_index(Bag *bag, int start);
void bag_set_end_tile_index(Bag *bag, int end);

XoshiroPRNG *bag_get_prng(Bag *bag);

void bag_add_letter(Bag *bag, MachineLetter letter, int player_draw_index);
bool bag_draw_letter(Bag *bag, MachineLetter letter, int player_draw_index);

MachineLetter bag_draw_random_letter(Bag *bag, int player_draw_index);
void bag_seed(Bag *bag, uint64_t seed);
void bag_normalize_window(Bag *bag);
void bag_reset(const LetterDistribution *ld, Bag *bag);
void bag_shuffle(Bag *bag);
void bag_increment_unseen_count(const Bag *bag, int *unseen_count);

#endif