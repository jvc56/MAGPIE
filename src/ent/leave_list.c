#include "leave_list.h"

#include "klv.h"
#include "leave_count_hashmap.h"
#include "rack.h"

#include "../util/util.h"

typedef struct LeaveListItem {
  int count_index;
  int count;
  double mean;
} LeaveListItem;

struct LeaveList {
  const KLV *klv;
  Rack *full_rack;
  Rack *subleave;
  double move_equity;
  // FIXME: get better names for these
  int number_of_leaves;
  int end_index_list_size;
  LeaveListItem *empty_leave;
  LeaveListItem **leaves_ordered_by_klv_index;
  LeaveListItem **leaves_ordered_by_count;
  LeaveCountHashMap *leave_count_hashmap;
};

LeaveListItem *leave_list_item_create(void) {
  LeaveListItem *item = malloc_or_die(sizeof(LeaveListItem));
  item->count = 0;
  item->mean = 0;
  return item;
}

LeaveList *leave_list_create(int number_of_leaves) {
  LeaveList *leave_list = malloc_or_die(sizeof(LeaveList));
  leave_list->number_of_leaves = number_of_leaves;
  leave_list->end_index_list_size = number_of_leaves;
  size_t leaves_malloc_size = sizeof(LeaveListItem *) * number_of_leaves;
  leave_list->leaves_ordered_by_klv_index = malloc_or_die(leaves_malloc_size);
  for (int i = 0; i < number_of_leaves; i++) {
    leave_list->leaves_ordered_by_klv_index[i] = leave_list_item_create();
  }

  leave_list->empty_leave = leave_list_item_create();

  leave_list->leaves_ordered_by_count = malloc_or_die(leaves_malloc_size);

  memory_copy(leave_list->leaves_ordered_by_count,
              leave_list->leaves_ordered_by_klv_index, leaves_malloc_size);

  leave_list->leave_count_hashmap =
      leave_count_hashmap_create(number_of_leaves);

  leave_count_hashmap_set(leave_list->leave_count_hashmap, 0,
                          number_of_leaves - 1);

  leave_list->subleave = rack_create(0);

  return leave_list;
}

void leave_list_destroy(LeaveList *leave_list) {
  for (int i = 0; i < leave_list->number_of_leaves; i++) {
    free(leave_list->leaves_ordered_by_klv_index[i]);
  }
  free(leave_list->leaves_ordered_by_klv_index);
  free(leave_list->leaves_ordered_by_count);
  free(leave_list->empty_leave);
  leave_count_hashmap_destroy(leave_list->leave_count_hashmap);
  rack_destroy(leave_list->subleave);
  free(leave_list);
}

void leave_list_item_increment_count(LeaveListItem *item, double equity) {
  item->count++;
  item->mean += item->mean + ((double)1 / item->count) * (equity - item->mean);
}

void leave_list_add_subleave(LeaveList *leave_list, int klv_index,
                             double equity) {
  LeaveListItem *item = leave_list->leaves_ordered_by_klv_index[klv_index];

  int old_count = item->count;

  leave_list_item_increment_count(item, equity);

  uint64_t old_end_index =
      leave_count_hashmap_get(leave_list->leave_count_hashmap, old_count);

  LeaveListItem *swapped_item =
      leave_list->leaves_ordered_by_count[old_end_index];

  int item_old_count_index = item->count_index;
  item->count_index = swapped_item->count_index;
  swapped_item->count_index = item_old_count_index;

  leave_list->leaves_ordered_by_count[old_end_index] = item;
  leave_list->leaves_ordered_by_count[item_old_count_index] = swapped_item;

  // Ensure new end index is correct
  leave_count_hashmap_set(leave_list->leave_count_hashmap, old_count,
                          old_end_index - 1);

  if (old_end_index == 0 ||
      leave_list->leaves_ordered_by_count[old_end_index - 1]->count !=
          old_count) {
    leave_count_hashmap_delete(leave_list->leave_count_hashmap, old_count);
  }
}

void generate_subleaves(LeaveList *leave_list, uint32_t node_index,
                        uint32_t word_index, uint8_t ml) {
  const uint32_t ld_size = rack_get_dist_size(leave_list->full_rack);
  while (ml < ld_size && rack_get_letter(leave_list->full_rack, ml) == 0) {
    ml++;
  }
  if (ml == ld_size) {
    if (!rack_is_empty(leave_list->full_rack)) {
      leave_list_add_subleave(leave_list, word_index - 1,
                              leave_list->move_equity);
    } else {
      leave_list_item_increment_count(leave_list->empty_leave,
                                      leave_list->move_equity);
    }
  } else {
    generate_subleaves(leave_list, node_index, word_index, ml + 1);
    const int num_this = rack_get_letter(leave_list->full_rack, ml);
    for (int i = 0; i < num_this; i++) {
      rack_add_letter(leave_list->subleave, ml);
      rack_take_letter(leave_list->full_rack, ml);
      uint32_t sibling_word_index;
      node_index = increment_node_to_ml(leave_list->klv, node_index, word_index,
                                        &sibling_word_index, ml);
      word_index = sibling_word_index;
      uint32_t child_word_index;
      node_index = follow_arc(leave_list->klv, node_index, word_index,
                              &child_word_index);
      word_index = child_word_index;
      generate_subleaves(leave_list, node_index, word_index, ml + 1);
    }

    rack_take_letters(leave_list->subleave, ml, num_this);
    rack_add_letters(leave_list->full_rack, ml, num_this);
  }
}

void leave_list_add_leave(LeaveList *leave_list, const KLV *klv,
                          Rack *full_rack, double move_equity) {
  leave_list->klv = klv;
  leave_list->full_rack = full_rack;
  leave_list->move_equity = move_equity;
  rack_copy_dist_size(leave_list->subleave, leave_list->full_rack);
  rack_reset(leave_list->subleave);
  generate_subleaves(leave_list, kwg_get_dawg_root_node_index(klv->kwg), 0, 0);
}

void leave_list_write_to_klv(LeaveList *leave_list, KLV *klv) {
  int number_of_leaves = leave_list->number_of_leaves;
  LeaveListItem **items = leave_list->leaves_ordered_by_klv_index;
  double average_leave_value = leave_list->empty_leave->mean;
  for (int i = 0; i < number_of_leaves; i++) {
    klv->leave_values[i] = items[i]->mean - average_leave_value;
  }
}