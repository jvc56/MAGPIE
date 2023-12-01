#ifndef LEAVE_RACK_H
#define LEAVE_RACK_H

struct LeaveRackList;
typedef struct LeaveRackList LeaveRackList;

LeaveRackList *create_leave_rack_list(int capacity, int distribution_size);
void destroy_leave_rack_list(LeaveRackList *lrl);
void reset_leave_rack_list(LeaveRackList *lrl);
void sort_leave_racks(LeaveRackList *lrl);

#endif