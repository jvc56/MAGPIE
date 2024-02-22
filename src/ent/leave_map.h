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
  int letter_base_index_map[MAX_ALPHABET_SIZE];
  int current_index;
} LeaveMap;

LeaveMap *leave_map_create(int rack_array_size);
void leave_map_destroy(LeaveMap *LeaveMap);
void leave_map_init(const Rack *rack, LeaveMap *leave_map);

static inline void leave_map_set_current_value(LeaveMap *leave_map, double value) {
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

static inline void leave_map_take_letter_and_update_current_index(LeaveMap *leave_map,
                                                           Rack *rack,
                                                           uint8_t letter) {
  rack_take_letter(rack, letter);
  leave_map_take_letter(leave_map, letter, rack_get_letter(rack, letter));
}

static inline void leave_map_add_letter_and_update_current_index(LeaveMap *leave_map,
                                                          Rack *rack,
                                                          uint8_t letter) {
  rack_add_letter(rack, letter);
  leave_map_add_letter(leave_map, letter, rack_get_letter(rack, letter) - 1);
}

#endif