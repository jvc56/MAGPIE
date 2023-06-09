#ifndef LEAVE_RACK_H
#define LEAVE_RACK_H

#include <stdint.h>

#include "constants.h"
#include "rack.h"

typedef struct LeaveRack {
    Rack * rack;
    int draws;
    double equity;
} LeaveRack;

typedef struct LeaveRackList {
    int count;
    int capacity;
    LeaveRack * spare_leave_rack;
    LeaveRack ** leave_racks;
} LeaveRackList;

LeaveRackList * create_leave_rack_list(int capacity, int distribution_size);
void destroy_leave_rack_list(LeaveRackList * lrl);
void insert_leave_rack(LeaveRackList * lrl, Rack * rack, int number_of_draws_for_leave, double equity);
LeaveRack * pop_leave_rack(LeaveRackList * lrl);
void reset_leave_rack_list(LeaveRackList * lrl);
void sort_leave_racks(LeaveRackList * lrl);

#endif