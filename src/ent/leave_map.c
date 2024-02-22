#include "leave_map.h"

#include <stdint.h>
#include <stdlib.h>

#include "../def/rack_defs.h"

#include "rack.h"

#include "../util/util.h"

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
  for (int i = 0; i < rack_get_dist_size(rack); i++) {
    if (rack_get_letter(rack, i) > 0) {
      leave_map->letter_base_index_map[i] = current_base_index;
      current_base_index += rack_get_letter(rack, i);
    }
  }
  leave_map->current_index = (1 << current_base_index) - 1;
}