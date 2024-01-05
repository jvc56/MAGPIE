#ifndef LEAVE_RACK_H
#define LEAVE_RACK_H

#include "rack.h"

typedef struct LeaveRack LeaveRack;

Rack *leave_rack_get_leave(const LeaveRack *leaveRack);
Rack *leave_rack_get_exchanged(const LeaveRack *leaveRack);
int leave_rack_get_draws(const LeaveRack *leaveRack);
double leave_rack_get_equity(const LeaveRack *leaveRack);

typedef struct LeaveRackList LeaveRackList;

LeaveRackList *leave_rack_list_create(int capacity, int distribution_size);
void leave_rack_list_destroy(LeaveRackList *lrl);
void leave_rack_list_sort(LeaveRackList *lrl);

int leave_rack_list_get_count(const LeaveRackList *leave_rack_list);
const LeaveRack *leave_rack_list_get_rack(const LeaveRackList *leave_rack_list,
                                          int index);

LeaveRack *leave_rack_list_pop_rack(LeaveRackList *lrl);
void leave_rack_list_insert_rack(const Rack *leave, const Rack *exchanged,
                                 int number_of_draws_for_leave, double equity,
                                 LeaveRackList *lrl);
#endif