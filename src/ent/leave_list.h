#ifndef LEAVE_LIST_H
#define LEAVE_LIST_H

#include "bag.h"
#include "encoded_rack.h"
#include "klv.h"
#include "rack.h"
#include "thread_control.h"

typedef struct LeaveList LeaveList;

LeaveList *leave_list_create(const LetterDistribution *ld, KLV *klv,
                             int target_leave_count);
void leave_list_destroy(LeaveList *leave_list);
void leave_list_reset(LeaveList *leave_list);
int leave_list_add_all_subleaves(LeaveList *leave_list, Rack *full_rack,
                                 double move_equity);
int leave_list_add_single_subleave(LeaveList *leave_list, const Rack *leave,
                                   double equity);
void leave_list_write_to_klv(LeaveList *leave_list);
void leave_list_get_rarest_leave(LeaveList *leave_list, Rack *rack);
int leave_list_get_target_leave_count(const LeaveList *leave_list);
int leave_list_get_leaves_below_target_count(const LeaveList *leave_list);
int leave_list_get_number_of_leaves(const LeaveList *leave_list);
int leave_list_get_lowest_leave_count(const LeaveList *leave_list);
uint64_t leave_list_get_count(const LeaveList *leave_list, int klv_index);
double leave_list_get_mean(const LeaveList *leave_list, int klv_index);
int leave_list_get_empty_leave_count(const LeaveList *leave_list);
double leave_list_get_empty_leave_mean(const LeaveList *leave_list);
void leave_list_seed(LeaveList *leave_list, ThreadControl *thread_control);
const EncodedRack *leave_list_get_encoded_rack(const LeaveList *leave_list,
                                               int klv_index);
void string_builder_add_most_or_least_common_leaves(
    StringBuilder *sb, const LeaveList *leave_list,
    const LetterDistribution *ld, int n, bool most_common);
#endif