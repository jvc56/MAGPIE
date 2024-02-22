#include <stdio.h>
#include <stdlib.h>

#include "leave_map.h"
#include "rack.h"
#include "util.h"

LeaveMap *create_leave_map(int rack_array_size) {
  LeaveMap *leave_map = malloc_or_die(sizeof(LeaveMap));
  leave_map->leave_values = (double *)malloc_or_die(128 * sizeof(double));
  leave_map->letter_base_index_map =
      (int *)malloc_or_die(rack_array_size * sizeof(int));
  leave_map->reversed_letter_bit_map =
      (int *)malloc_or_die(rack_array_size * sizeof(int));
  return leave_map;
}

void destroy_leave_map(LeaveMap *leave_map) {
  free(leave_map->leave_values);
  free(leave_map->letter_base_index_map);
  free(leave_map->reversed_letter_bit_map);
  free(leave_map);
}

void take_letter_and_update_current_index(LeaveMap *leave_map, Rack *rack,
                                          uint8_t letter) {
  take_letter_from_rack(rack, letter);
  int base_index = leave_map->letter_base_index_map[letter];
  int offset = rack->array[letter];
  int bit_index = base_index + offset;
  leave_map->current_index &= ~(1 << bit_index);
}

void add_letter_and_update_current_index(LeaveMap *leave_map, Rack *rack,
                                         uint8_t letter) {
  add_letter_to_rack(rack, letter);
  int base_index = leave_map->letter_base_index_map[letter];
  int offset = rack->array[letter] - 1;
  int bit_index = base_index + offset;
  leave_map->current_index |= 1 << bit_index;
}

void take_letter_and_update_complement_index(LeaveMap *leave_map, Rack *rack,
                                             uint8_t letter) {
  take_letter_from_rack(rack, letter);
  int base_index = leave_map->letter_base_index_map[letter];
  int offset = rack->array[letter];
  int bit_index = base_index + offset;
  int reversed_bit = leave_map->reversed_letter_bit_map[bit_index];
  leave_map->current_index |= reversed_bit;
}

void add_letter_and_update_complement_index(LeaveMap *leave_map, Rack *rack,
                                            uint8_t letter) {
  add_letter_to_rack(rack, letter);
  int base_index = leave_map->letter_base_index_map[letter];
  int offset = rack->array[letter] - 1;
  int bit_index = base_index + offset;
  int reversed_bit = leave_map->reversed_letter_bit_map[bit_index];
  leave_map->current_index &= ~reversed_bit;
}

void set_current_value(LeaveMap *leave_map, double value) {
  leave_map->leave_values[leave_map->current_index] = value;
}

double get_current_value(LeaveMap *leave_map) {
  return leave_map->leave_values[leave_map->current_index];
}

void init_leave_map(LeaveMap *leave_map, Rack *rack) {
  int current_base_index = 0;
  for (int i = 0; i < rack->array_size; i++) {
    if (rack->array[i] > 0) {
      leave_map->letter_base_index_map[i] = current_base_index;
      for (int j = 0; j < rack->array[i]; j++) {
        int bit_index = current_base_index + rack->array[i] - j - 1;
        int bit = 1 << bit_index;
        leave_map->reversed_letter_bit_map[current_base_index + j] = bit;
      }
      current_base_index += rack->array[i];
    }
  }
  leave_map->current_index = (1 << current_base_index) - 1;
}
