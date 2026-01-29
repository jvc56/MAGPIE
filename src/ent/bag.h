#ifndef BAG_H
#define BAG_H

#include "letter_distribution.h"
#include "xoshiro.h"
#include <stdbool.h>
#include <stdint.h>

enum { MAX_BAG_SIZE = 1000 };

typedef struct Bag Bag;

Bag *bag_create(const LetterDistribution *ld, uint64_t seed);
void bag_destroy(Bag *bag);
void bag_copy(Bag *dst, const Bag *src);
Bag *bag_duplicate(const Bag *bag);

int bag_get_letters(const Bag *bag);
int bag_get_letter(const Bag *bag, MachineLetter ml);
bool bag_is_empty(const Bag *bag);

// Accessor functions for bag tile indices (used for incremental play/unplay)
int bag_get_start_tile_index(const Bag *bag);
void bag_set_start_tile_index(Bag *bag, int index);
int bag_get_end_tile_index(const Bag *bag);
void bag_set_end_tile_index(Bag *bag, int index);

void bag_add_letter(Bag *bag, MachineLetter letter, int player_draw_index);
bool bag_draw_letter(Bag *bag, MachineLetter letter, int player_draw_index);

MachineLetter bag_draw_random_letter(Bag *bag, int player_draw_index);
void bag_seed(Bag *bag, uint64_t seed);
void bag_reset(const LetterDistribution *ld, Bag *bag);
void bag_shuffle(Bag *bag);
void bag_increment_unseen_count(const Bag *bag, int *unseen_count);

#endif