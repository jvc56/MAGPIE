#ifndef LEAVE_LIST_H
#define LEAVE_LIST_H

#include "klv.h"
#include "rack.h"

typedef struct LeaveList LeaveList;

LeaveList *leave_list_create(int number_of_leaves);
void leave_list_destroy(LeaveList *leave_list);
void leave_list_add_leave(LeaveList *leave_list, const KLV *klv,
                          Rack *full_rack, double move_equity);
void leave_list_write_to_klv(LeaveList *leave_list, KLV *klv);

#endif