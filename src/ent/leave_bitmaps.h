#ifndef LEAVE_BITMAPS_H
#define LEAVE_BITMAPS_H

#include <limits.h>

#include "bag.h"
#include "rack.h"

#define BITMAP_UNIT uint64_t

typedef struct LeaveBitMaps {
  const LetterDistribution *ld;
  int unit_size_in_bits;
  int units_per_bitmap;
  int size;
  BITMAP_UNIT *bag;
  BITMAP_UNIT *rack;
  BITMAP_UNIT *leaves;
} LeaveBitMaps;

typedef enum {
  BITMAP_BAG,
  BITMAP_RACK,
  BITMAP_LEAVES,
} bitmap_t;

static inline LeaveBitMaps *leave_bitmaps_create(const LetterDistribution *ld,
                                                 int size) {
  LeaveBitMaps *lb = malloc(sizeof(LeaveBitMaps));

  lb->ld = ld;

  int bag_size = 0;
  int ld_size = ld_get_size(ld);
  for (int i = 0; i < ld_size; i++) {
    bag_size += ld_get_dist(ld, i);
  }

  int unit_size = sizeof(BITMAP_UNIT);
  lb->unit_size_in_bits = unit_size * CHAR_BIT;
  lb->units_per_bitmap =
      (bag_size + lb->unit_size_in_bits - 1) / lb->unit_size_in_bits;
  lb->size = size;
  lb->bag = calloc_or_die(lb->units_per_bitmap, unit_size);
  lb->rack = calloc_or_die(lb->units_per_bitmap, unit_size);
  lb->leaves = calloc_or_die(lb->size * lb->units_per_bitmap, unit_size);
  return lb;
}

static inline void leave_bitmaps_destroy(LeaveBitMaps *lb) {
  free(lb->bag);
  free(lb->rack);
  free(lb->leaves);
  free(lb);
}

static inline BITMAP_UNIT *get_bitmaps(LeaveBitMaps *lb,
                                       bitmap_t bitmaps_type) {
  uint64_t *bitmaps = NULL;
  switch (bitmaps_type) {
  case BITMAP_BAG:
    bitmaps = lb->bag;
    break;
  case BITMAP_RACK:
    bitmaps = lb->rack;
    break;
  case BITMAP_LEAVES:
    bitmaps = lb->leaves;
    break;
  }
  return bitmaps;
}

static inline void leave_bitmaps_set_bit(LeaveBitMaps *lb,
                                         bitmap_t bitmaps_type,
                                         int unit_start_index, int bit_index) {
  uint64_t *bitmaps = get_bitmaps(lb, bitmaps_type);
  const int unit_index = unit_start_index + bit_index / lb->unit_size_in_bits;
  const int unit_offset = bit_index % lb->unit_size_in_bits;
  bitmaps[unit_index] |= (BITMAP_UNIT)1 << unit_offset;
}

static inline bool leave_bitmaps_get_bit(LeaveBitMaps *lb,
                                         bitmap_t bitmaps_type,
                                         int unit_start_index, int bit_index) {
  uint64_t *bitmaps = get_bitmaps(lb, bitmaps_type);
  const int unit_index = unit_start_index + bit_index / lb->unit_size_in_bits;
  const int unit_offset = bit_index % lb->unit_size_in_bits;
  return bitmaps[unit_index] & ((BITMAP_UNIT)1 << unit_offset);
}

static inline void leave_bitmaps_set_leave_internal(LeaveBitMaps *lb,
                                                    bitmap_t bitmaps_type,
                                                    const Rack *rack,
                                                    int index) {
  // First, reset the bitmap.
  uint64_t *bitmaps = get_bitmaps(lb, bitmaps_type);
  for (int i = 0; i < lb->units_per_bitmap; i++) {
    bitmaps[index * lb->units_per_bitmap + i] = 0;
  }
  const int dist_size = rack_get_dist_size(rack);
  int bitmaps_start_index = index * lb->units_per_bitmap;
  int bit_index = 0;
  for (int i = 0; i < dist_size; i++) {
    const int num_letter = rack_get_letter(rack, i);
    const int num_letter_dist = ld_get_dist(lb->ld, i);
    for (int j = 0; j < num_letter; j++) {
      leave_bitmaps_set_bit(lb, bitmaps_type, bitmaps_start_index, bit_index);
      bit_index++;
    }
    bit_index += num_letter_dist - num_letter;
  }
}

static inline void leave_bitmaps_set_leave(LeaveBitMaps *lb, const Rack *rack,
                                           int index) {
  leave_bitmaps_set_leave_internal(lb, BITMAP_LEAVES, rack, index);
}

// Draws the minimum number of letters required to ensure that the
// rack contains the the leave specified by index.
static inline void leave_bitmaps_draw_to_leave(LeaveBitMaps *lb, Bag *bag,
                                               Rack *rack,
                                               int player_draw_index,
                                               int index) {
  const int dist_size = rack_get_dist_size(rack);
  int unit_start_index = index * lb->units_per_bitmap;
  int bit_index = 0;
  for (int i = 0; i < dist_size; i++) {
    const int num_letter_in_dist = ld_get_dist(lb->ld, i);
    const int num_letter_in_rack = rack_get_letter(rack, i);
    int num_letter_in_leave = 0;
    for (int j = 0; j < num_letter_in_dist; j++) {
      if (leave_bitmaps_get_bit(lb, BITMAP_LEAVES, unit_start_index,
                                bit_index)) {
        num_letter_in_leave++;
      } else {
        break;
      }
      bit_index++;
    }
    // Move the bit index to the next letter in the distribution.
    bit_index += num_letter_in_dist - num_letter_in_leave;
    const int num_letter_to_draw = num_letter_in_leave - num_letter_in_rack;
    if (num_letter_to_draw > 0) {
      rack_add_letters(rack, i, num_letter_to_draw);
      if (bag) {
        bag_draw_letters(bag, i, num_letter_to_draw, player_draw_index);
      }
    }
  }
}

// Swaps the leaves at the given indexes.
static inline void leave_bitmaps_swap_leaves(LeaveBitMaps *lb, int i, int j) {
  BITMAP_UNIT temp;
  int i_start_index = i * lb->units_per_bitmap;
  int j_start_index = j * lb->units_per_bitmap;
  uint64_t *bitmaps = get_bitmaps(lb, BITMAP_LEAVES);
  for (int unit_index = 0; unit_index < lb->units_per_bitmap; unit_index++) {
    temp = bitmaps[i_start_index + unit_index];
    bitmaps[i_start_index + unit_index] = bitmaps[j_start_index + unit_index];
    bitmaps[j_start_index + unit_index] = temp;
  }
}

// This gets the Hamming weight for x, which in this case is number of letters
// in the unit. This algorithm works better when most bits in x are 0, which
// is what we would normally expect since RACK_SIZE is typically 7 and the
// BITMAP_UNIT is typically 64 bits.
static inline int get_number_of_letters_in_unit(BITMAP_UNIT x) {
  int count;
  for (count = 0; x; count++) {
    x &= x - 1;
  }
  return count;
}

// Returns true and sets the rack to the first available leave
// by index.
// Returns false if there is no valid leave.
static inline bool
leave_bitmaps_draw_first_available_subrack(LeaveBitMaps *lb, Bag *bag,
                                           Rack *rack, int player_draw_index) {
  const int dist_size = rack_get_dist_size(rack);
  for (int i = 0; i < dist_size; i++) {
    rack_add_letters(rack, i, bag_get_letter(bag, i));
  }
  leave_bitmaps_set_leave_internal(lb, BITMAP_BAG, rack, 0);
  for (int i = 0; i < dist_size; i++) {
    rack_take_letters(rack, i, bag_get_letter(bag, i));
  }
  leave_bitmaps_set_leave_internal(lb, BITMAP_RACK, rack, 0);
  const int number_of_bitmaps = lb->size;
  const int units_per_bitmap = lb->units_per_bitmap;
  const uint64_t *bag_bitmap = get_bitmaps(lb, BITMAP_BAG);
  const uint64_t *rack_bitmap = get_bitmaps(lb, BITMAP_RACK);
  const uint64_t *leave_bitmaps = get_bitmaps(lb, BITMAP_LEAVES);
  for (int i = 0; i < number_of_bitmaps; i++) {
    bool leave_is_available = true;
    int total_letters = 0;
    for (int j = 0; j < units_per_bitmap; j++) {
      const BITMAP_UNIT bag_unit = bag_bitmap[j];
      const BITMAP_UNIT rack_unit = rack_bitmap[j];
      const BITMAP_UNIT leave_unit = leave_bitmaps[i * units_per_bitmap + j];
      total_letters += get_number_of_letters_in_unit(leave_unit | rack_unit);
      if (((leave_unit & bag_unit) != leave_unit) ||
          (total_letters > (RACK_SIZE))) {
        leave_is_available = false;
        break;
      }
    }
    if (leave_is_available) {
      leave_bitmaps_draw_to_leave(lb, bag, rack, player_draw_index, i);
      return true;
    }
  }
  return false;
}

#endif