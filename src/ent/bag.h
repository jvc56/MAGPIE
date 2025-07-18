#ifndef BAG_H
#define BAG_H

#include <stdbool.h>
#include <stdint.h>

#include "letter_distribution.h"
#include "xoshiro.h"

#define MAX_BAG_SIZE 1000

typedef struct Bag Bag;

Bag *bag_create(const LetterDistribution *ld, uint64_t seed);
void bag_destroy(Bag *bag);
void bag_copy(Bag *dst, const Bag *src);
Bag *bag_duplicate(const Bag *bag);

int bag_get_size(const Bag *bag);
int bag_get_tiles(const Bag *bag);
int bag_get_letter(const Bag *bag, MachineLetter ml);
XoshiroPRNG *bag_get_prng(const Bag *bag);
bool bag_is_empty(const Bag *bag);

void bag_add_letter(Bag *bag, MachineLetter letter, int player_draw_index);
bool bag_draw_letter(Bag *bag, MachineLetter letter, int player_draw_index);
bool bag_draw_letters(Bag *bag, MachineLetter letter, int num_letters,
                      int player_draw_index);
MachineLetter bag_draw_random_letter(Bag *bag, int player_draw_index);
void bag_seed(Bag *bag, uint64_t seed);
void bag_reset(const LetterDistribution *ld, Bag *bag);
void bag_shuffle(Bag *bag);
void bag_increment_unseen_count(const Bag *bag, int *unseen_count);
#endif