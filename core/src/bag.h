#ifndef BAG_H
#define BAG_H

#include <stdint.h>

#include "letter_distribution.h"
#include "rack.h"
#include "string_util.h"
#include "xoshiro.h"

#define MAX_BAG_SIZE 1000

struct Bag;
typedef struct Bag Bag;

int get_tiles_remaining(const Bag *bag);
bool bag_is_empty(const Bag *bag);
uint8_t draw_random_letter(Bag *bag, int player_index);
void add_letter(Bag *bag, uint8_t letter);
void remove_letter(Bag *bag, uint8_t letter);
void seed_bag_for_worker(Bag *bag, uint64_t seed, int worker_index);
void add_bag_to_rack(const Bag *bag, Rack *rack);
void destroy_bag(Bag *bag);
Bag *create_bag(const LetterDistribution *letter_distribution);
Bag *copy_bag(const Bag *bag);
void update_bag(Bag *bag, const LetterDistribution *letter_distribution);
void copy_bag_into(Bag *dst, const Bag *src);
void reseed_prng(Bag *bag, uint64_t seed);
void reset_bag(Bag *bag, const LetterDistribution *letter_distribution);
void shuffle(Bag *bag);
void string_builder_add_bag(const Bag *bag,
                            const LetterDistribution *letter_distribution,
                            size_t len, StringBuilder *string_builder);
#endif