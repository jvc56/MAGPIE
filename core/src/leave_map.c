#include <stdlib.h>
#include <stdio.h>

#include "rack.h"
#include "leave_map.h"

LeaveMap * create_leave_map(int rack_size) {
    int number_of_values = 1 << RACK_SIZE;
    LeaveMap * leave_map = malloc(sizeof(LeaveMap));
    leave_map->leave_values = (float *) malloc(number_of_values*sizeof(float));
    return leave_map;
}

void destroy_leave_map(LeaveMap * leave_map) {
    free(leave_map->leave_values);
    free(leave_map);
}

LeaveMap * load_leave_map(LeaveMap * leave_map, Rack * rack) {

}