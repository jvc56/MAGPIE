#include "leave_list.h"

#include "bag.h"
#include "game.h"
#include "klv.h"
#include "leave_count_hashmap.h"
#include "rack.h"

#include "../util/util.h"

// FIXME: remove
#include "../str/rack_string.h"

#define MAX_LEAVE_SIZE (RACK_SIZE - 1)

typedef struct LeaveListItem {
  int count_index;
  int count;
  double mean;
  Rack leave;
} LeaveListItem;

struct LeaveList {
  KLV *klv;
  Rack *full_rack;
  Rack *subleave;
  double move_equity;
  int number_of_leaves;
  LeaveListItem *empty_leave;
  LeaveListItem **leaves_ordered_by_klv_index;
  LeaveListItem **leaves_ordered_by_count;
  LeaveCountHashMap *leave_count_hashmap;
};

LeaveListItem *leave_list_item_create(int count_index) {
  LeaveListItem *item = malloc_or_die(sizeof(LeaveListItem));
  item->count = 0;
  item->mean = 0;
  item->count_index = count_index;
  return item;
}

int leave_list_set_item_leaves(LeaveList *leave_list,
                               const LetterDistribution *ld, Rack *bag_as_rack,
                               Rack *leave, uint32_t node_index,
                               uint32_t word_index, uint8_t ml) {
  int number_of_set_leaves = 0;
  const uint32_t ld_size = ld_get_size(ld);
  while (ml < ld_size && rack_get_letter(bag_as_rack, ml) == 0) {
    ml++;
  }
  const int number_of_letters_in_leave = rack_get_total_letters(leave);
  if (ml == ld_size) {
    if (number_of_letters_in_leave > 0) {
      if (word_index == KLV_UNFOUND_INDEX) {
        log_fatal("word index not found in klv: %u", word_index);
      }
      rack_copy(&leave_list->leaves_ordered_by_klv_index[word_index - 1]->leave,
                leave);
      number_of_set_leaves++;
    }
  } else {
    number_of_set_leaves += leave_list_set_item_leaves(
        leave_list, ld, bag_as_rack, leave, node_index, word_index, ml + 1);
    int num_ml_to_add = rack_get_letter(bag_as_rack, ml);
    if (number_of_letters_in_leave + num_ml_to_add > MAX_LEAVE_SIZE) {
      num_ml_to_add = MAX_LEAVE_SIZE - number_of_letters_in_leave;
    }
    for (int i = 0; i < num_ml_to_add; i++) {
      rack_add_letter(leave, ml);
      rack_take_letter(bag_as_rack, ml);
      uint32_t sibling_word_index;
      node_index = increment_node_to_ml(leave_list->klv, node_index, word_index,
                                        &sibling_word_index, ml);
      word_index = sibling_word_index;
      uint32_t child_word_index;
      node_index = follow_arc(leave_list->klv, node_index, word_index,
                              &child_word_index);
      word_index = child_word_index;
      number_of_set_leaves += leave_list_set_item_leaves(
          leave_list, ld, bag_as_rack, leave, node_index, word_index, ml + 1);
    }
    rack_take_letters(leave, ml, num_ml_to_add);
    rack_add_letters(bag_as_rack, ml, num_ml_to_add);
  }
  return number_of_set_leaves;
}

LeaveList *leave_list_create(const LetterDistribution *ld, KLV *klv) {
  LeaveList *leave_list = malloc_or_die(sizeof(LeaveList));
  leave_list->klv = klv;
  leave_list->number_of_leaves = klv_get_number_of_leaves(klv);
  size_t leaves_malloc_size =
      sizeof(LeaveListItem *) * leave_list->number_of_leaves;
  leave_list->leaves_ordered_by_klv_index = malloc_or_die(leaves_malloc_size);
  for (int i = 0; i < leave_list->number_of_leaves; i++) {
    leave_list->leaves_ordered_by_klv_index[i] = leave_list_item_create(i);
  }

  leave_list->empty_leave = leave_list_item_create(-1);

  leave_list->leaves_ordered_by_count = malloc_or_die(leaves_malloc_size);

  memory_copy(leave_list->leaves_ordered_by_count,
              leave_list->leaves_ordered_by_klv_index, leaves_malloc_size);

  leave_list->leave_count_hashmap =
      leave_count_hashmap_create(leave_list->number_of_leaves);

  leave_count_hashmap_set(leave_list->leave_count_hashmap, 0,
                          leave_list->number_of_leaves - 1);

  const int ld_size = ld_get_size(ld);

  leave_list->subleave = rack_create(ld_size);

  Bag *bag = bag_create(ld);
  Rack *bag_as_rack = rack_create(ld_size);

  for (int i = 0; i < ld_size; i++) {
    int number_of_tiles = bag_get_letter(bag, i);
    rack_add_letters(bag_as_rack, i, number_of_tiles);
  }

  bag_destroy(bag);

  const int number_of_set_leaves = leave_list_set_item_leaves(
      leave_list, ld, bag_as_rack, leave_list->subleave,
      kwg_get_dawg_root_node_index(leave_list->klv->kwg), 0, 0);

  if (number_of_set_leaves != leave_list->number_of_leaves) {
    log_fatal("Did not set the correct number of leaves:\nnumber of set "
              "leaves: %d\nnumber of leaves: %d\n",
              number_of_set_leaves, leave_list->number_of_leaves);
  }

  rack_destroy(bag_as_rack);

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
    if (!rack_is_empty(leave_list->subleave)) {
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

void leave_list_add_leave(LeaveList *leave_list, KLV *klv, Rack *full_rack,
                          double move_equity) {
  leave_list->klv = klv;
  leave_list->full_rack = full_rack;
  leave_list->move_equity = move_equity;
  rack_reset(leave_list->subleave);
  generate_subleaves(leave_list, kwg_get_dawg_root_node_index(klv->kwg), 0, 0);
}

void leave_list_write_to_klv(LeaveList *leave_list) {
  int number_of_leaves = leave_list->number_of_leaves;
  LeaveListItem **items = leave_list->leaves_ordered_by_klv_index;
  double average_leave_value = leave_list->empty_leave->mean;
  KLV *klv = leave_list->klv;
  for (int i = 0; i < number_of_leaves; i++) {
    klv->leave_values[i] = items[i]->mean - average_leave_value;
  }
}

int leave_list_get_number_of_leaves(const LeaveList *leave_list) {
  return leave_list->number_of_leaves;
}

const Rack *leave_list_get_rack_by_count_index(const LeaveList *leave_list,
                                               int count_index) {
  return &leave_list->leaves_ordered_by_count[count_index]->leave;
}

int leave_list_get_empty_leave_count(const LeaveList *leave_list) {
  return leave_list->empty_leave->count;
}

double leave_list_get_empty_leave_mean(const LeaveList *leave_list) {
  return leave_list->empty_leave->mean;
}