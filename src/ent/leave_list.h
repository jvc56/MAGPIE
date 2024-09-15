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
void leave_list_reset(LeaveList *leave_list, int target_leave_count);
void leave_list_add_all_subleaves(LeaveList *leave_list, Rack *full_rack,
                                  Rack *subleave, double move_equity);
void leave_list_add_single_subleave(LeaveList *leave_list, const Rack *leave,
                                    double equity);
void leave_list_write_to_klv(LeaveList *leave_list);
bool leave_list_get_rare_leave(LeaveList *leave_list, XoshiroPRNG *prng,
                               Rack *rack);
int leave_list_get_target_leave_count(const LeaveList *leave_list);
int leave_list_get_leaves_below_target_count(const LeaveList *leave_list);
int leave_list_get_number_of_leaves(const LeaveList *leave_list);
uint64_t leave_list_get_count(const LeaveList *leave_list, int klv_index);
double leave_list_get_mean(const LeaveList *leave_list, int klv_index);
int leave_list_get_empty_leave_count(const LeaveList *leave_list);
double leave_list_get_empty_leave_mean(const LeaveList *leave_list);
const EncodedRack *leave_list_get_encoded_rack(const LeaveList *leave_list,
                                               int klv_index);
#endif