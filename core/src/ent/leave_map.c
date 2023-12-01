#include <stdint.h>

#include "leave_map.h"

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