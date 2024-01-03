#ifndef BAG_H
#define BAG_H

#include <stdbool.h>

#include "letter_distribution.h"

#define MAX_BAG_SIZE 1000

struct Bag;
typedef struct Bag Bag;

Bag *bag_create(const LetterDistribution *letter_distribution);
void bag_destroy(Bag *bag);
void bag_copy(Bag *dst, const Bag *src);
Bag *bag_duplicate(const Bag *bag);

int bag_get_tiles(const Bag *bag);
int bag_get_letter(const Bag *bag, uint8_t ml);
bool bag_is_empty(const Bag *bag);

void bag_add_letter(Bag *bag, uint8_t letter, int player_draw_index);
void bag_draw_letter(Bag *bag, uint8_t letter, int player_draw_index);
uint8_t bag_draw_random_letter(Bag *bag, int player_draw_index);
void bag_reseed(Bag *bag, uint64_t seed);
void bag_seed_for_worker(Bag *bag, uint64_t seed, int worker_index);
void bag_reset(const LetterDistribution *letter_distribution, Bag *bag);
void bag_shuffle(Bag *bag);

#endif