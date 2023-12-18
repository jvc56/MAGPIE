#ifndef BAG_H
#define BAG_H

#include <stdbool.h>

#include "letter_distribution.h"
#include "rack.h"

#define MAX_BAG_SIZE 1000

struct Bag;
typedef struct Bag Bag;

Bag *create_bag(const LetterDistribution *letter_distribution);
void destroy_bag(Bag *bag);
void bag_copy(Bag *dst, const Bag *src);
Bag *bag_duplicate(const Bag *bag);

void add_letter(Bag *bag, uint8_t letter, int player_draw_index);
bool bag_is_empty(const Bag *bag);
void draw_letter(Bag *bag, uint8_t letter, int player_draw_index);
uint8_t draw_random_letter(Bag *bag, int player_draw_index);
int get_tiles_remaining(const Bag *bag);
void reseed_prng(Bag *bag, uint64_t seed);
void reset_bag(const LetterDistribution *letter_distribution, Bag *bag);
void seed_bag_for_worker(Bag *bag, uint64_t seed, int worker_index);
void shuffle(Bag *bag);
void add_bag_to_rack(const Bag *bag, Rack *rack);

#endif