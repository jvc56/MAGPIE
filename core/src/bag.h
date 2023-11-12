#ifndef BAG_H
#define BAG_H

#include <stdint.h>

#include "letter_distribution.h"
#include "string_util.h"
#include "xoshiro.h"

#define MAX_BAG_SIZE 1000
typedef struct Bag {
  int size;
  uint8_t *tiles;
  int last_tile_index;
  XoshiroPRNG *prng;
} Bag;

void add_letter(Bag *bag, uint8_t letter);
void draw_letter(Bag *bag, uint8_t letter);
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