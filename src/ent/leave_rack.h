#ifndef LEAVE_RACK_H
#define LEAVE_RACK_H

#include "rack.h"

typedef struct LeaveRack LeaveRack;

void leave_rack_get_leave(const LeaveRack *leave_rack, Rack *rack_to_update);
void leave_rack_get_exchanged(const LeaveRack *leave_rack,
                              Rack *rack_to_update);
int leave_rack_get_draws(const LeaveRack *leave_rack);
Equity leave_rack_get_equity(const LeaveRack *leave_rack);

typedef struct LeaveRackList LeaveRackList;

LeaveRackList *leave_rack_list_create(int capacity);
void leave_rack_list_destroy(LeaveRackList *lrl);
void leave_rack_list_sort(LeaveRackList *lrl);

int leave_rack_list_get_count(const LeaveRackList *leave_rack_list);
const LeaveRack *leave_rack_list_get_rack(const LeaveRackList *leave_rack_list,
                                          int index);

LeaveRack *leave_rack_list_pop_rack(LeaveRackList *lrl);
void leave_rack_list_insert_rack(const Rack *leave, const Rack *exchanged,
                                 int number_of_draws_for_leave, Equity equity,
                                 LeaveRackList *lrl);
void leave_rack_list_insert_leave_rack(const LeaveRack *leave_rack,
                                       LeaveRackList *lrl);
#endif