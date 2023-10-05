#include <stdio.h>
#include <stdlib.h>

#include "constants.h"
#include "leave_rack.h"
#include "util.h"

LeaveRack *create_leave_rack(int distribution_size) {
  LeaveRack *leave_rack = malloc_or_die(sizeof(LeaveRack));
  leave_rack->draws = 0;
  leave_rack->equity = 0;
  leave_rack->leave = create_rack(distribution_size);
  leave_rack->exchanged = create_rack(distribution_size);
  return leave_rack;
}

void destroy_leave_rack(LeaveRack *leave_rack) {
  destroy_rack(leave_rack->leave);
  destroy_rack(leave_rack->exchanged);
  free(leave_rack);
}

LeaveRackList *create_leave_rack_list(int capacity, int distribution_size) {
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

void destroy_leave_rack_list(LeaveRackList *lrl) {
  for (int i = 0; i < lrl->capacity + 1; i++) {
    destroy_leave_rack(lrl->leave_racks[i]);
  }
  destroy_leave_rack(lrl->spare_leave_rack);
  free(lrl->leave_racks);
  free(lrl);
}

void reset_leave_rack_list(LeaveRackList *lrl) { lrl->count = 0; }

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

void insert_leave_rack(LeaveRackList *lrl, Rack *leave, Rack *exchanged,
                       int number_of_draws_for_leave, double equity) {
  reset_rack(lrl->spare_leave_rack->leave);
  for (int i = 0; i < leave->array_size; i++) {
    for (int j = 0; j < leave->array[i]; j++) {
      add_letter_to_rack(lrl->spare_leave_rack->leave, i);
    }
  }
  lrl->spare_leave_rack->draws = number_of_draws_for_leave;
  lrl->spare_leave_rack->equity = equity;
  if (exchanged && !exchanged->empty) {
    reset_rack(lrl->spare_leave_rack->exchanged);
    for (int i = 0; i < exchanged->array_size; i++) {
      for (int j = 0; j < exchanged->array[i]; j++) {
        add_letter_to_rack(lrl->spare_leave_rack->exchanged, i);
      }
    }
  }

  LeaveRack *swap = lrl->leave_racks[lrl->count];
  lrl->leave_racks[lrl->count] = lrl->spare_leave_rack;
  lrl->spare_leave_rack = swap;

  up_heapify_leave_rack(lrl, lrl->count);
  lrl->count++;

  if (lrl->count > lrl->capacity) {
    pop_leave_rack(lrl);
  }
}

LeaveRack *pop_leave_rack(LeaveRackList *lrl) {
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

void sort_leave_racks(LeaveRackList *lrl) {
  int number_of_leave_racks = lrl->count;
  for (int i = 1; i < number_of_leave_racks; i++) {
    LeaveRack *leave_rack = pop_leave_rack(lrl);
    // Use a swap var to preserve the spare leave pointer
    LeaveRack *swap = lrl->leave_racks[lrl->count];
    lrl->leave_racks[lrl->count] = leave_rack;
    lrl->spare_leave_rack = swap;
  }
}
