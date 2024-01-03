#include <stdint.h>
#include <stdlib.h>

#include "../def/rack_defs.h"

#include "../util/util.h"

#include "leave_map.h"
#include "rack.h"

struct LeaveMap {
  int rack_array_size;
  double *leave_values;
  int *letter_base_index_map;
  int current_index;
};

LeaveMap *leave_map_create(int rack_array_size) {
  int number_of_values = 1 << RACK_SIZE;
  LeaveMap *leave_map = malloc_or_die(sizeof(LeaveMap));
  leave_map->rack_array_size = rack_array_size;
  leave_map->leave_values =
      (double *)malloc_or_die(number_of_values * sizeof(double));
  leave_map->letter_base_index_map =
      (int *)malloc_or_die(leave_map->rack_array_size * sizeof(int));
  return leave_map;
}

void leave_map_destroy(LeaveMap *leave_map) {
  free(leave_map->leave_values);
  free(leave_map->letter_base_index_map);
  free(leave_map);
}

void leave_map_update(LeaveMap *leave_map, int new_rack_array_size) {
  if (leave_map->rack_array_size != new_rack_array_size) {
    free(leave_map->letter_base_index_map);
    leave_map->rack_array_size = new_rack_array_size;
    leave_map->letter_base_index_map =
        (int *)malloc_or_die(leave_map->rack_array_size * sizeof(int));
  }
}

double *leave_map_get_leave_values(const LeaveMap *leave_map) {
  return leave_map->leave_values;
}

void leave_map_set_current_value(LeaveMap *leave_map, double value) {
  leave_map->leave_values[leave_map->current_index] = value;
}

double leave_map_get_current_value(const LeaveMap *leave_map) {
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

void leave_map_take_letter_and_update_current_index(LeaveMap *leave_map,
                                                    Rack *rack,
                                                    uint8_t letter) {
  rack_take_letter(rack, letter);
  leave_map_take_letter(leave_map, letter, rack_get_letter(rack, letter));
}

void leave_map_add_letter_and_update_current_index(LeaveMap *leave_map,
                                                   Rack *rack, uint8_t letter) {
  rack_add_letter(rack, letter);
  leave_map_add_letter(leave_map, letter,
                       rack_get_letter(rack, letter) - 1);
}

void leave_map_init(const Rack *rack, LeaveMap *leave_map) {
  int current_base_index = 0;
  for (int i = 0; i < rack_get_dist_size(rack); i++) {
    if (rack_get_letter(rack, i) > 0) {
      leave_map->letter_base_index_map[i] = current_base_index;
      current_base_index += rack_get_letter(rack, i);
    }
  }
  leave_map->current_index = (1 << current_base_index) - 1;
}