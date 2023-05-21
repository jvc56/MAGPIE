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

#endif