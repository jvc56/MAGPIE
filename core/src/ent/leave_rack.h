#ifndef LEAVE_RACK_H
#define LEAVE_RACK_H

#include "rack.h"

struct LeaveRack;
typedef struct LeaveRack LeaveRack;

struct LeaveRackList;
typedef struct LeaveRackList LeaveRackList;

Rack *leave_rack_get_leave(const LeaveRack *leaveRack);
Rack *leave_rack_get_exchanged(const LeaveRack *leaveRack);
int leave_rack_get_draws(const LeaveRack *leaveRack);
double leave_rack_get_equity(const LeaveRack *leaveRack);
const LeaveRack *get_leave_rack(const LeaveRackList *leave_rack_list,
                                int index);

LeaveRackList *create_leave_rack_list(int capacity, int distribution_size);
void destroy_leave_rack_list(LeaveRackList *lrl);
void reset_leave_rack_list(LeaveRackList *lrl);
void sort_leave_racks(LeaveRackList *lrl);
int get_leave_rack_list_capacity(const LeaveRackList *leave_rack_list);
int get_leave_rack_list_count(const LeaveRackList *leave_rack_list);
LeaveRack *pop_leave_rack(LeaveRackList *lrl);
void insert_leave_rack(const Rack *leave, const Rack *exchanged,
                       LeaveRackList *lrl, int number_of_draws_for_leave,
                       double equity);
#endif