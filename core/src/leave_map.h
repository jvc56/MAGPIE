#ifndef LEAVE_MAP_H
#define LEAVE_MAP_H

#include <stdint.h>

#include "kwg.h"
#include "rack.h"

typedef struct LeaveMap {
    float * leave_values;
    int current_bit_map;
} LeaveMap;

LeaveMap * create_leave_map(int rack_size);
void destroy_leave_map(LeaveMap * LeaveMap);

#endif