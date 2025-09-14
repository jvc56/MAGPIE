#include "leave_rack.h"

#include "../util/io_util.h"
#include "encoded_rack.h"
#include "equity.h"
#include "rack.h"
#include <stdlib.h>
#include <string.h>

struct LeaveRack {
  EncodedRack leave;
  EncodedRack exchanged;
  int draws;
  Equity equity;
};

// Used by the inference_results
// to store the most common leaves
// (and possibly exchanges) for
// an inference.
struct LeaveRackList {
  int count;
  int capacity;
  int max_all_time_capacity;
  LeaveRack *spare_leave_rack;
  LeaveRack **leave_racks;
};

void leave_rack_get_leave(const LeaveRack *leave_rack, Rack *rack_to_update) {
  rack_decode(&leave_rack->leave, rack_to_update);
}

void leave_rack_get_exchanged(const LeaveRack *leave_rack,
                              Rack *rack_to_update) {
  rack_decode(&leave_rack->exchanged, rack_to_update);
}

int leave_rack_get_draws(const LeaveRack *leave_rack) {
  return leave_rack->draws;
}

Equity leave_rack_get_equity(const LeaveRack *leave_rack) {
  return leave_rack->equity;
}

int leave_rack_list_get_count(const LeaveRackList *leave_rack_list) {
  return leave_rack_list->count;
}

const LeaveRack *leave_rack_list_get_rack(const LeaveRackList *leave_rack_list,
                                          int index) {
  return leave_rack_list->leave_racks[index];
}

LeaveRack *leave_rack_create(void) {
  return calloc_or_die(1, sizeof(LeaveRack));
}

void leave_rack_destroy(LeaveRack *leave_rack) {
  if (!leave_rack) {
    return;
  }
  free(leave_rack);
}

size_t leave_rack_get_alloc_size(const int new_capacity) {
  // Use capacity + 1 to temporarily hold a new insertion
  // before popping it.
  return sizeof(LeaveRack *) * (new_capacity + 1);
}

LeaveRackList *leave_rack_list_create(int capacity) {
  LeaveRackList *lrl = malloc_or_die(sizeof(LeaveRackList));
  lrl->count = 0;
  lrl->capacity = capacity;
  lrl->max_all_time_capacity = capacity;
  lrl->spare_leave_rack = leave_rack_create();
  // Use capacity + 1 to temporarily hold a new insertion
  // before popping it.
  lrl->leave_racks = malloc_or_die(leave_rack_get_alloc_size(capacity));
  for (int i = 0; i < lrl->capacity + 1; i++) {
    lrl->leave_racks[i] = leave_rack_create();
  }
  return lrl;
}

void leave_rack_list_destroy(LeaveRackList *lrl) {
  if (!lrl) {
    return;
  }
  for (int i = 0; i < lrl->max_all_time_capacity + 1; i++) {
    leave_rack_destroy(lrl->leave_racks[i]);
  }
  leave_rack_destroy(lrl->spare_leave_rack);
  free(lrl->leave_racks);
  free(lrl);
}

void leave_rack_list_reset(LeaveRackList *lrl, int capacity) {
  if (!lrl) {
    return;
  }
  lrl->count = 0;
  if (lrl->max_all_time_capacity < capacity) {
    lrl->leave_racks =
        realloc_or_die(lrl->leave_racks, leave_rack_get_alloc_size(capacity));
    for (int i = lrl->max_all_time_capacity + 1; i < capacity + 1; i++) {
      lrl->leave_racks[i] = leave_rack_create();
    }
    lrl->max_all_time_capacity = capacity;
  }
  lrl->capacity = capacity;
}

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

  if (left >= lrl->count || left < 0) {
    left = -1;
  }
  if (right >= lrl->count || right < 0) {
    right = -1;
  }

  if (left != -1 &&
      lrl->leave_racks[left]->draws < lrl->leave_racks[parent_node]->draws) {
    min = left;
  } else {
    min = parent_node;
  }
  if (right != -1 &&
      lrl->leave_racks[right]->draws < lrl->leave_racks[min]->draws) {
    min = right;
  }

  if (min != parent_node) {
    temp = lrl->leave_racks[min];
    lrl->leave_racks[min] = lrl->leave_racks[parent_node];
    lrl->leave_racks[parent_node] = temp;
    down_heapify_leave_rack(lrl, min);
  }
}

void leave_rack_list_insert_spare_and_up_heapify(LeaveRackList *lrl) {
  LeaveRack *swap = lrl->leave_racks[lrl->count];
  lrl->leave_racks[lrl->count] = lrl->spare_leave_rack;
  lrl->spare_leave_rack = swap;

  up_heapify_leave_rack(lrl, lrl->count);
  lrl->count++;

  if (lrl->count > lrl->capacity) {
    leave_rack_list_pop_rack(lrl);
  }
}

void leave_rack_list_insert_leave_rack(const LeaveRack *leave_rack,
                                       LeaveRackList *lrl) {
  memcpy(lrl->spare_leave_rack, leave_rack, sizeof(LeaveRack));
  leave_rack_list_insert_spare_and_up_heapify(lrl);
}

void leave_rack_list_insert_rack(const Rack *leave, const Rack *exchanged,
                                 int number_of_draws_for_leave, Equity equity,
                                 LeaveRackList *lrl) {
  rack_encode(leave, &lrl->spare_leave_rack->leave);
  lrl->spare_leave_rack->draws = number_of_draws_for_leave;
  lrl->spare_leave_rack->equity = equity;
  if (exchanged && !rack_is_empty(exchanged)) {
    rack_encode(exchanged, &lrl->spare_leave_rack->exchanged);
  }
  leave_rack_list_insert_spare_and_up_heapify(lrl);
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
// to a descending sorted array. The count stays
// constant.
void leave_rack_list_sort(LeaveRackList *lrl) {
  int number_of_leave_racks = lrl->count;
  for (int i = 1; i < number_of_leave_racks; i++) {
    LeaveRack *leave_rack = leave_rack_list_pop_rack(lrl);
    // Use a swap var to preserve the spare leave pointer
    LeaveRack *swap = lrl->leave_racks[lrl->count];
    lrl->leave_racks[lrl->count] = leave_rack;
    lrl->spare_leave_rack = swap;
  }
  lrl->count = number_of_leave_racks;
}
