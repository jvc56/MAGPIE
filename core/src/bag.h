#ifndef BAG_H
#define BAG_H

#include <stdint.h>

#include "letter_distribution.h"
#include "string_util.h"
#include "xoshiro.h"

#define BAG_SIZE 100
typedef struct Bag {
  uint8_t tiles[BAG_SIZE];
  int last_tile_index;
  XoshiroPRNG *prng;
} Bag;

void add_letter(Bag *bag, uint8_t letter);
void draw_letter(Bag *bag, uint8_t letter);
void destroy_bag(Bag *bag);
Bag *create_bag(LetterDistribution *letter_distribution);
Bag *copy_bag(Bag *bag);
void copy_bag_into(Bag *dst, Bag *src);
void reseed_prng(Bag *bag, uint64_t seed);
void reset_bag(Bag *bag, LetterDistribution *letter_distribution);
void shuffle(Bag *bag);
void string_builder_add_bag(Bag *bag, LetterDistribution *letter_distribution,
                            size_t len, StringBuilder *string_builder);
#endif