#ifndef BAG_BITMAPS_H
#define BAG_BITMAPS_H

#include "bag.h"
#include "rack.h"

typedef struct BagBitMaps BagBitMaps;

BagBitMaps *bag_bitmaps_create(const LetterDistribution *ld, int size);
void bag_bitmaps_destroy(BagBitMaps *bag_bitmaps);
void bag_bitmaps_set_bitmap(BagBitMaps *bag_bitmaps, const Rack *rack,
                            int index);
void bag_bitmaps_swap(BagBitMaps *bag_bitmaps, int i, int j);
bool bag_bitmaps_draw_first_available_subrack(BagBitMaps *bag_bitmaps, Bag *bag,
                                              Rack *rack,
                                              int player_draw_index);

#endif