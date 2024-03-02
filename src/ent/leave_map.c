#include "leave_map.h"

#include <stdint.h>
#include <stdlib.h>

#include "../def/rack_defs.h"
#include "../util/util.h"
#include "rack.h"

LeaveMap *leave_map_create(int rack_array_size) {
  LeaveMap *leave_map = malloc_or_die(sizeof(LeaveMap));
  leave_map->rack_array_size = rack_array_size;
  return leave_map;
}

void leave_map_destroy(LeaveMap *leave_map) {
  if (!leave_map) {
    return;
  }
  free(leave_map);
}

void leave_map_init(const Rack *rack, LeaveMap *leave_map) {
  int current_base_index = 0;
  leave_map->rack_array_size = rack_get_dist_size(rack);
  for (int i = 0; i < leave_map->rack_array_size; i++) {
    const int num_this = rack_get_letter(rack, i);
    if (num_this > 0) {
      leave_map->letter_base_index_map[i] = current_base_index;
      for (int j = 0; j < num_this; j++) {
        int bit_index = current_base_index + num_this - j - 1;
        int bit = 1 << bit_index;
        leave_map->reversed_letter_bit_map[current_base_index + j] = bit;
      }
      current_base_index += num_this;
    }
  }
  leave_map->current_index = (1 << current_base_index) - 1;
}