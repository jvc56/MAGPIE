#ifndef LEAVE_LIST_H
#define LEAVE_LIST_H

#include "klv.h"
#include "rack.h"

typedef struct LeaveList LeaveList;

LeaveList *leave_list_create(const LetterDistribution *ld, KLV *klv);
void leave_list_destroy(LeaveList *leave_list);
void leave_list_add_leave(LeaveList *leave_list, KLV *klv, Rack *full_rack,
                          double move_equity);
int leave_list_get_number_of_leaves(const LeaveList *leave_list);
const Rack *leave_list_get_rack_by_count_index(const LeaveList *leave_list,
                                               int count_index);
void leave_list_write_to_klv(LeaveList *leave_list);
int leave_list_get_empty_leave_count(const LeaveList *leave_list);
double leave_list_get_empty_leave_mean(const LeaveList *leave_list);

#endif