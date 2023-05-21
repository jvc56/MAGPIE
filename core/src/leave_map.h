#ifndef LEAVE_MAP_H
#define LEAVE_MAP_H

#include <stdint.h>

#include "kwg.h"
#include "rack.h"

typedef struct LeaveMap {
    float * leave_values;
    int * letter_base_index_map;
    int current_index;
} LeaveMap;

LeaveMap * create_leave_map(int rack_array_size);
void destroy_leave_map(LeaveMap * LeaveMap);
void init_leave_map(LeaveMap * leave_map, Rack * rack);
void take_letter_and_update_current_value(LeaveMap * leave_map, Rack * rack, uint8_t letter);
void add_letter_and_update_current_value(LeaveMap * leave_map, Rack * rack, uint8_t letter);
void set_current_value(LeaveMap * leave_map, float value);
float get_current_value(LeaveMap * leave_map);

#endif