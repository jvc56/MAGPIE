#ifndef LEAVE_LIST_H
#define LEAVE_LIST_H

#include "bag.h"
#include "klv.h"
#include "rack.h"
#include "thread_control.h"

typedef struct LeaveList LeaveList;

LeaveList *leave_list_create(const LetterDistribution *ld, KLV *klv,
                             int target_min_leave_count);
void leave_list_destroy(LeaveList *leave_list);
void leave_list_reset(LeaveList *leave_list);
int leave_list_add_leave(LeaveList *leave_list, Rack *full_rack,
                         double move_equity);
int leave_list_add_subleave(LeaveList *leave_list, const Rack *subleave,
                            double equity);
void leave_list_write_to_klv(LeaveList *leave_list);
bool leave_list_draw_rare_leave(LeaveList *leave_list,
                                const LetterDistribution *ld, Bag *bag,
                                Rack *player_rack, int player_draw_index,
                                Rack *rare_leave);
int leave_list_get_target_min_leave_count(const LeaveList *leave_list);
int leave_list_get_leaves_under_target_min_count(const LeaveList *leave_list);
int leave_list_get_number_of_leaves(const LeaveList *leave_list);
int leave_list_get_lowest_leave_count(const LeaveList *leave_list);
uint64_t leave_list_get_count(const LeaveList *leave_list, int count_index);
double leave_list_get_mean(const LeaveList *leave_list, int count_index);
int leave_list_get_empty_leave_count(const LeaveList *leave_list);
double leave_list_get_empty_leave_mean(const LeaveList *leave_list);

void string_builder_add_most_or_least_common_leaves(
    StringBuilder *sb, const LeaveList *leave_list,
    const LetterDistribution *ld, int n, bool most_common);
#endif