#include <stdint.h>

#include "../def/rack_defs.h"

#include "leave_map.h"
#include "rack.h"

struct LeaveMap {
  uint32_t rack_array_size;
  double *leave_values;
  int *letter_base_index_map;
  int current_index;
};

void update_leave_map(LeaveMap *leave_map, int new_rack_array_size) {
  if (leave_map->rack_array_size != (uint32_t)new_rack_array_size) {
    free(leave_map->letter_base_index_map);
    leave_map->rack_array_size = new_rack_array_size;
    leave_map->letter_base_index_map =
        (int *)malloc_or_die(leave_map->rack_array_size * sizeof(int));
  }
}

LeaveMap *create_leave_map(int rack_array_size) {
  int number_of_values = 1 << RACK_SIZE;
  LeaveMap *leave_map = malloc_or_die(sizeof(LeaveMap));
  leave_map->rack_array_size = rack_array_size;
  leave_map->leave_values =
      (double *)malloc_or_die(number_of_values * sizeof(double));
  leave_map->letter_base_index_map =
      (int *)malloc_or_die(leave_map->rack_array_size * sizeof(int));
  return leave_map;
}

void destroy_leave_map(LeaveMap *leave_map) {
  free(leave_map->leave_values);
  free(leave_map->letter_base_index_map);
  free(leave_map);
}

void set_current_value(LeaveMap *leave_map, double value) {
  leave_map->leave_values[leave_map->current_index] = value;
}

double get_current_value(const LeaveMap *leave_map) {
  return leave_map->leave_values[leave_map->current_index];
}

int leave_map_get_current_index(const LeaveMap *leave_map) {
  return leave_map->current_index;
}

void leave_map_take_letter(LeaveMap *leave_map, uint8_t letter,
                           int number_of_letter_on_rack) {
  int base_index = leave_map->letter_base_index_map[letter];
  int offset = number_of_letter_on_rack;
  int bit_index = base_index + offset;
  leave_map->current_index &= ~(1 << bit_index);
}

void leave_map_add_letter(LeaveMap *leave_map, uint8_t letter,
                          int number_of_letter_on_rack) {
  int base_index = leave_map->letter_base_index_map[letter];
  int offset = number_of_letter_on_rack;
  int bit_index = base_index + offset;
  leave_map->current_index |= 1 << bit_index;
}

void init_leave_map(const Rack *rack, LeaveMap *leave_map) {
  int current_base_index = 0;
  for (int i = 0; i < get_array_size(rack); i++) {
    if (get_number_of_letter(rack, i) > 0) {
      leave_map->letter_base_index_map[i] = current_base_index;
      current_base_index += get_number_of_letter(rack, i);
    }
  }
  leave_map->current_index = (1 << current_base_index) - 1;
}