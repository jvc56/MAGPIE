#include "leave_rack.h"

#include <stdlib.h>

#include "rack.h"

#include "../util/util.h"

struct LeaveRack {
  Rack *leave;
  Rack *exchanged;
  int draws;
  double equity;
};

struct LeaveRackList {
  int count;
  int capacity;
  LeaveRack *spare_leave_rack;
  LeaveRack **leave_racks;
};

Rack *leave_rack_get_leave(const LeaveRack *leaveRack) {
  return leaveRack->leave;
}

Rack *leave_rack_get_exchanged(const LeaveRack *leaveRack) {
  return leaveRack->exchanged;
}

int leave_rack_get_draws(const LeaveRack *leaveRack) {
  return leaveRack->draws;
}

double leave_rack_get_equity(const LeaveRack *leaveRack) {
  return leaveRack->equity;
}

int leave_rack_list_get_count(const LeaveRackList *leave_rack_list) {
  return leave_rack_list->count;
}

int leave_rack_list_get_capacity(const LeaveRackList *leave_rack_list) {
  return leave_rack_list->capacity;
}

const LeaveRack *leave_rack_list_get_rack(const LeaveRackList *leave_rack_list,
                                          int index) {
  return leave_rack_list->leave_racks[index];
}

LeaveRack *create_leave_rack(int distribution_size) {
  LeaveRack *leave_rack = malloc_or_die(sizeof(LeaveRack));
  leave_rack->draws = 0;
  leave_rack->equity = 0;
  leave_rack->leave = rack_create(distribution_size);
  leave_rack->exchanged = rack_create(distribution_size);
  return leave_rack;
}

void destroy_leave_rack(LeaveRack *leave_rack) {
  rack_destroy(leave_rack->leave);
  rack_destroy(leave_rack->exchanged);
  free(leave_rack);
}

LeaveRackList *leave_rack_list_create(int capacity, int distribution_size) {
  LeaveRackList *lrl = malloc_or_die(sizeof(LeaveRackList));
  lrl->count = 0;
  lrl->capacity = capacity;
  lrl->spare_leave_rack = create_leave_rack(distribution_size);
  // Use capacity + 1 to temporarily hold a new insertion
  // before popping it.
  lrl->leave_racks = malloc_or_die((sizeof(LeaveRack *)) * (lrl->capacity + 1));
  for (int i = 0; i < lrl->capacity + 1; i++) {
    lrl->leave_racks[i] = create_leave_rack(distribution_size);
  }
  return lrl;
}

void leave_rack_list_destroy(LeaveRackList *lrl) {
  for (int i = 0; i < lrl->capacity + 1; i++) {
    destroy_leave_rack(lrl->leave_racks[i]);
  }
  destroy_leave_rack(lrl->spare_leave_rack);
  free(lrl->leave_racks);
  free(lrl);
}

void leave_rack_list_reset(LeaveRackList *lrl) { lrl->count = 0; }

void up_heapify_leave_rack(LeaveRackList *lrl, int index) {
  LeaveRack *temp;
  int parent_node = (index - 1) / 2;
  if (lrl->leave_racks[parent_node]->draws > lrl->leave_racks[index]->draws) {
    temp = lrl->leave_racks[parent_node];
    lrl->leave_racks[parent_node] = lrl->leave_racks[index];
    lrl->leave_racks[index] = temp;
    up_heapify_leave_rack(lrl, parent_node);
  }
}

void down_heapify_leave_rack(LeaveRackList *lrl, int parent_node) {
  int left = parent_node * 2 + 1;
  int right = parent_node * 2 + 2;
  int min;
  LeaveRack *temp;

  if (left >= lrl->count || left < 0)
    left = -1;
  if (right >= lrl->count || right < 0)
    right = -1;

  if (left != -1 &&
      lrl->leave_racks[left]->draws < lrl->leave_racks[parent_node]->draws)
    min = left;
  else
    min = parent_node;
  if (right != -1 &&
      lrl->leave_racks[right]->draws < lrl->leave_racks[min]->draws)
    min = right;

  if (min != parent_node) {
    temp = lrl->leave_racks[min];
    lrl->leave_racks[min] = lrl->leave_racks[parent_node];
    lrl->leave_racks[parent_node] = temp;
    down_heapify_leave_rack(lrl, min);
  }
}

void leave_rack_list_insert_rack(const Rack *leave, const Rack *exchanged,
                                 LeaveRackList *lrl,
                                 int number_of_draws_for_leave, double equity) {
  rack_reset(lrl->spare_leave_rack->leave);
  for (int i = 0; i < rack_get_dist_size(leave); i++) {
    for (int j = 0; j < rack_get_letter(leave, i); j++) {
      rack_add_letter(lrl->spare_leave_rack->leave, i);
    }
  }
  lrl->spare_leave_rack->draws = number_of_draws_for_leave;
  lrl->spare_leave_rack->equity = equity;
  if (exchanged && !rack_is_empty(exchanged)) {
    rack_reset(lrl->spare_leave_rack->exchanged);
    for (int i = 0; i < rack_get_dist_size(exchanged); i++) {
      for (int j = 0; j < rack_get_letter(exchanged, i); j++) {
        rack_add_letter(lrl->spare_leave_rack->exchanged, i);
      }
    }
  }

  LeaveRack *swap = lrl->leave_racks[lrl->count];
  lrl->leave_racks[lrl->count] = lrl->spare_leave_rack;
  lrl->spare_leave_rack = swap;

  up_heapify_leave_rack(lrl, lrl->count);
  lrl->count++;

  if (lrl->count > lrl->capacity) {
    leave_rack_list_pop_rack(lrl);
  }
}

LeaveRack *leave_rack_list_pop_rack(LeaveRackList *lrl) {
  if (lrl->count == 1) {
    lrl->count--;
    return lrl->leave_racks[0];
  }
  LeaveRack *swap = lrl->spare_leave_rack;
  lrl->spare_leave_rack = lrl->leave_racks[0];
  lrl->leave_racks[0] = lrl->leave_racks[lrl->count - 1];
  lrl->leave_racks[lrl->count - 1] = swap;

  lrl->count--;
  down_heapify_leave_rack(lrl, 0);
  return lrl->spare_leave_rack;
}

// Converts the LeaveRackList from a min heap
// to a descending sorted array.
void leave_rack_list_sort(LeaveRackList *lrl) {
  int number_of_leave_racks = lrl->count;
  for (int i = 1; i < number_of_leave_racks; i++) {
    LeaveRack *leave_rack = leave_rack_list_pop_rack(lrl);
    // Use a swap var to preserve the spare leave pointer
    LeaveRack *swap = lrl->leave_racks[lrl->count];
    lrl->leave_racks[lrl->count] = leave_rack;
    lrl->spare_leave_rack = swap;
  }
}
