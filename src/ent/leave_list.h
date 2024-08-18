#ifndef LEAVE_LIST_H
#define LEAVE_LIST_H

#include "bag.h"
#include "klv.h"
#include "rack.h"

typedef struct LeaveList LeaveList;

LeaveList *leave_list_create(const LetterDistribution *ld, KLV *klv,
                             int max_rare_draw_count);
void leave_list_destroy(LeaveList *leave_list);
void leave_list_reset(LeaveList *leave_list);
int leave_list_add_leave(LeaveList *leave_list, Rack *full_rack,
                         double move_equity);
int leave_list_add_subleave(LeaveList *leave_list, const Rack *subleave,
                            double equity);
void leave_list_write_to_klv(LeaveList *leave_list);
bool leave_list_draw_rarest_available_leave(LeaveList *leave_list, Bag *bag,
                                            Rack *rack, Rack *empty_rack,
                                            int player_draw_index);
int leave_list_get_min_count(const LeaveList *leave_list);

int leave_list_get_number_of_leaves(const LeaveList *leave_list);
uint64_t leave_list_get_count(const LeaveList *leave_list, int count_index);
double leave_list_get_mean(const LeaveList *leave_list, int count_index);
int leave_list_get_empty_leave_count(const LeaveList *leave_list);
double leave_list_get_empty_leave_mean(const LeaveList *leave_list);
int leave_list_get_count_index(const LeaveList *leave_list, int klv_index);
Rack *get_new_bag_as_rack(const LetterDistribution *ld);

void string_builder_add_most_or_least_common_leaves(
    StringBuilder *sb, const LeaveList *leave_list,
    const LetterDistribution *ld, int n, bool most_common);
#endif