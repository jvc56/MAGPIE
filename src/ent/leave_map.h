#ifndef LEAVE_MAP_H
#define LEAVE_MAP_H

#include <stdint.h>

#include "rack.h"

// The LeaveMap struct is used by
// movegen to lookup the leave value
// for a move in O(1) time. It can be
// viewed as a complement to the Gordon
// Generator in that as the play on the
// board changes one tile at a time
// in the generator, the leave changes
// one tile at a time in the LeaveMap.
typedef struct LeaveMap {
  int rack_array_size;
  double leave_values[1 << RACK_SIZE];
  uint8_t letter_base_index_map[MAX_ALPHABET_SIZE];
  uint64_t reversed_letter_bit_map[RACK_SIZE];
  int current_index;
} LeaveMap;

LeaveMap *leave_map_create(int rack_array_size);
void leave_map_destroy(LeaveMap *LeaveMap);
void leave_map_init(const Rack *rack, LeaveMap *leave_map);

static inline void leave_map_set_current_index(LeaveMap *leave_map,
                                               int current_index) {
  leave_map->current_index = current_index;
}

static inline void leave_map_set_current_value(LeaveMap *leave_map,
                                               double value) {
  leave_map->leave_values[leave_map->current_index] = value;
}

static inline double leave_map_get_current_value(const LeaveMap *leave_map) {
  return leave_map->leave_values[leave_map->current_index];
}

static inline int leave_map_get_current_index(const LeaveMap *leave_map) {
  return leave_map->current_index;
}

static inline void leave_map_take_letter(LeaveMap *leave_map, uint8_t letter,
                                         int number_of_letter_on_rack) {
  int base_index = leave_map->letter_base_index_map[letter];
  int offset = number_of_letter_on_rack;
  int bit_index = base_index + offset;
  leave_map->current_index &= ~(1 << bit_index);
}

static inline void leave_map_add_letter(LeaveMap *leave_map, uint8_t letter,
                                        int number_of_letter_on_rack) {
  int base_index = leave_map->letter_base_index_map[letter];
  int offset = number_of_letter_on_rack;
  int bit_index = base_index + offset;
  leave_map->current_index |= 1 << bit_index;
}

static inline void leave_map_take_letter_and_update_current_index(
    LeaveMap *leave_map, Rack *rack, uint8_t letter) {
  rack_take_letter(rack, letter);
  leave_map_take_letter(leave_map, letter, rack_get_letter(rack, letter));
}

static inline void leave_map_add_letter_and_update_current_index(
    LeaveMap *leave_map, Rack *rack, uint8_t letter) {
  rack_add_letter(rack, letter);
  leave_map_add_letter(leave_map, letter, rack_get_letter(rack, letter) - 1);
}

// These are used while looking up leave values for subsets of a rack to
// popluate leave_values. The index is updated differently because we are
// iterating over what we keep rather than what we use, and so while it is
// the 'complement' of the subset in that respect, it is not as simple as
// using the complement of the bitmask.
//
// Each tile on the rack has a unique bit associated with it, and for
// leave_map_add_letter the bit index is
//
//   letter_base_index_map[letter] + number_of_letter_on_rack
//
// so for example with the rack MOOORRT, the first O would be at index 1,
// second at 2, third at 3. But for the "complement index" we need to do
// them in the reverse order, i.e. we're changing the _last_ O _first_, which
// means the bit index depends on the number of that letter on the full rack,
// which is why a little table for these is generated upfront.
static inline void leave_map_take_letter_and_update_complement_index(
    LeaveMap *leave_map, Rack *rack, uint8_t letter) {
  rack_take_letter(rack, letter);
  const int base_index = leave_map->letter_base_index_map[letter];
  const int offset = rack->array[letter];
  const int bit_index = base_index + offset;
  const int reversed_bit = leave_map->reversed_letter_bit_map[bit_index];
  leave_map->current_index |= reversed_bit;
}

static inline void leave_map_add_letter_and_update_complement_index(
    LeaveMap *leave_map, Rack *rack, uint8_t letter) {
  rack_add_letter(rack, letter);
  const int base_index = leave_map->letter_base_index_map[letter];
  const int offset = rack->array[letter] - 1;
  const int bit_index = base_index + offset;
  const int reversed_bit = leave_map->reversed_letter_bit_map[bit_index];
  leave_map->current_index &= ~reversed_bit;
}
#endif