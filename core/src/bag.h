#ifndef BAG_H
#define BAG_H

#include <stdint.h>

#include "constants.h"
#include "letter_distribution.h"

typedef struct Bag {
  uint8_t tiles[BAG_SIZE];
  int last_tile_index;
} Bag;

void add_letter(Bag *bag, uint8_t letter);
void draw_letter(Bag *bag, uint8_t letter);
void destroy_bag(Bag *bag);
Bag *create_bag(LetterDistribution *letter_distribution);
Bag *copy_bag(Bag *bag);
void copy_bag_into(Bag *dst, Bag *src);
void reset_bag(Bag *bag, LetterDistribution *letter_distribution);
void shuffle(Bag *bag);

#endif