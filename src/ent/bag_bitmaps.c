#include "bag_bitmaps.h"

#include <limits.h>

#include "bag.h"
#include "rack.h"

#define BAG_BITMAPS_UNIT uint64_t

struct BagBitMaps {
  const LetterDistribution *ld;
  int unit_size_in_bits;
  int units_per_bag_bitmap;
  int number_of_bag_bitmaps;
  // The entry at index 0 is the bag which is
  // compared to the other entries.
  BAG_BITMAPS_UNIT *bitmaps;
};

BagBitMaps *bag_bitmaps_create(const LetterDistribution *ld,
                               int number_of_bitmaps) {
  BagBitMaps *bag_bitmaps = malloc(sizeof(BagBitMaps));

  bag_bitmaps->ld = ld;

  int bag_size = 0;
  int ld_size = ld_get_size(ld);
  for (int i = 0; i < ld_size; i++) {
    bag_size += ld_get_dist(ld, i);
  }

  int unit_size = sizeof(BAG_BITMAPS_UNIT);
  bag_bitmaps->unit_size_in_bits = unit_size * CHAR_BIT;
  bag_bitmaps->units_per_bag_bitmap = bag_size / bag_bitmaps->unit_size_in_bits;
  bag_bitmaps->number_of_bag_bitmaps = number_of_bitmaps + 1;
  bag_bitmaps->bitmaps = calloc_or_die(bag_bitmaps->number_of_bag_bitmaps *
                                           bag_bitmaps->units_per_bag_bitmap,
                                       unit_size);
  return bag_bitmaps;
}

void bag_bitmaps_destroy(BagBitMaps *bag_bitmaps) {
  free(bag_bitmaps->bitmaps);
  free(bag_bitmaps);
}

void bag_bitmaps_set_bit(BagBitMaps *bag_bitmaps, int bag_bitmap_start_index,
                         int bit_index) {
  const int unit_index =
      bag_bitmap_start_index + bit_index / bag_bitmaps->unit_size_in_bits;
  const int unit_offset = bit_index % bag_bitmaps->unit_size_in_bits;
  bag_bitmaps->bitmaps[unit_index] |= (BAG_BITMAPS_UNIT)1 << unit_offset;
}

bool bag_bitmaps_get_bit(const BagBitMaps *bag_bitmaps,
                         int bag_bitmap_start_index, int bit_index) {
  const int unit_index =
      bag_bitmap_start_index + bit_index / bag_bitmaps->unit_size_in_bits;
  const int unit_offset = bit_index % bag_bitmaps->unit_size_in_bits;
  return bag_bitmaps->bitmaps[unit_index] &
         ((BAG_BITMAPS_UNIT)1 << unit_offset);
}

void bag_bitmaps_set_bits(BagBitMaps *bag_bitmaps, Rack *rack, int index) {
  const int dist_size = rack_get_dist_size(rack);
  int bag_bitmap_start_index = index * bag_bitmaps->units_per_bag_bitmap;
  int bit_index = 0;
  for (int i = 0; i < dist_size; i++) {
    const int num_letter = rack_get_letter(rack, i);
    const int num_letter_dist = ld_get_dist(bag_bitmaps->ld, i);
    for (int j = 0; j < num_letter; j++) {
      bag_bitmaps_set_bit(bag_bitmaps, bag_bitmap_start_index, bit_index);
      bit_index++;
    }
    bit_index += num_letter_dist - num_letter;
  }
}

void bag_bitmaps_set_rack_internal(BagBitMaps *bag_bitmaps, Rack *rack,
                                   int index) {
  const int dist_size = rack_get_dist_size(rack);
  int bag_bitmap_start_index = index * bag_bitmaps->units_per_bag_bitmap;
  int bit_index = 0;
  for (int i = 0; i < dist_size; i++) {
    const int num_letter_dist = ld_get_dist(bag_bitmaps->ld, i);
    int num_set = 0;
    for (int j = 0; j < num_letter_dist; j++) {
      if (bag_bitmaps_get_bit(bag_bitmaps, bag_bitmap_start_index, bit_index)) {
        num_set++;
      } else {
        break;
      }
    }
    bit_index += num_letter_dist - num_set;
    rack_add_letters(rack, i, num_set);
  }
}

void bag_bitmaps_set_rack(BagBitMaps *bag_bitmaps, Rack *rack, int index) {
  // Use +1 to account for the bag bitmap at index 0 since
  // this function is exposed to the API and clients do not
  // know about the bag bitmap at index 0.
  bag_bitmaps_set_rack_internal(bag_bitmaps, rack, index + 1);
}

// Swap two bag bitmaps
void bag_bitmaps_swap(BagBitMaps *bag_bitmaps, int i, int j) {
  BAG_BITMAPS_UNIT temp;
  // Use +1 to account for the bag bitmap at index 0 since
  // this function is exposed to the API and clients do not
  // know about the bag bitmap at index 0.
  int i_start_index = (i + 1) * bag_bitmaps->units_per_bag_bitmap;
  int j_start_index = (j + 1) * bag_bitmaps->units_per_bag_bitmap;
  for (int unit_index = 0; unit_index < bag_bitmaps->units_per_bag_bitmap;
       unit_index++) {
    temp = bag_bitmaps->bitmaps[i_start_index + unit_index];
    bag_bitmaps->bitmaps[i_start_index + unit_index] =
        bag_bitmaps->bitmaps[j_start_index + unit_index];
    bag_bitmaps->bitmaps[j_start_index + unit_index] = temp;
  }
}

// Returns true and sets the rack to the first available subrack
// by index.
// Returns false if there is no valid subrack.
bool bag_bitmaps_get_first_subrack(BagBitMaps *bag_bitmaps, const Bag *bag,
                                   Rack *rack) {
  const int dist_size = rack_get_dist_size(rack);
  for (int i = 0; i < dist_size; i++) {
    rack_add_letters(rack, i, bag_get_letter(bag, i));
  }
  bag_bitmaps_set_bits(bag_bitmaps, rack, 0);
  for (int i = 0; i < dist_size; i++) {
    rack_take_letters(rack, i, bag_get_letter(bag, i));
  }

  const int number_of_bitmaps = bag_bitmaps->number_of_bag_bitmaps;
  const int units_per_bag_bitmap = bag_bitmaps->units_per_bag_bitmap;
  for (int i = 1; i < number_of_bitmaps; i++) {
    bool valid_subrack = true;
    for (int j = 0; j < units_per_bag_bitmap; j++) {
      const BAG_BITMAPS_UNIT bag_unit =
          bag_bitmaps->bitmaps[0 * units_per_bag_bitmap + j];
      const BAG_BITMAPS_UNIT subrack_unit =
          bag_bitmaps->bitmaps[i * units_per_bag_bitmap + j];
      if ((subrack_unit & bag_unit) != subrack_unit) {
        valid_subrack = false;
        break;
      }
    }
    if (valid_subrack) {
      bag_bitmaps_set_rack_internal(bag_bitmaps, rack, i);
      return true;
    }
  }
  return false;
}