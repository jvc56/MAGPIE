#include "leave_list.h"

#include "bag.h"
#include "game.h"
#include "klv.h"
#include "leave_bitmaps.h"
#include "leave_count_hashmap.h"
#include "rack.h"
#include "thread_control.h"
#include "xoshiro.h"

#include "../str/rack_string.h"

#include "../util/util.h"

#define MAX_LEAVE_SIZE (RACK_SIZE - 1)

typedef struct LeaveListItem {
  // Index of this item in the leave list items ordered by count.
  int count_index;
  int count;
  double mean;
} LeaveListItem;

struct LeaveList {
  // Owned by the caller
  KLV *klv;
  Rack *full_rack;
  // Owned by this struct
  Rack *subleave;
  double move_equity;
  int number_of_leaves;
  // Leaves with this count are no longer considered
  // rare and are excluded from forced draws.
  int target_min_leave_count;
  int leaves_under_target_min_count;
  LeaveListItem *empty_leave;
  LeaveListItem **leaves_ordered_by_klv_index;
  LeaveListItem **leaves_ordered_by_count;
  LeaveCountHashMap *leave_count_hashmap;
  LeaveBitMaps *leave_bitmaps;
  XoshiroPRNG *prng;
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
      leave_bitmaps_set_leave(leave_list->leave_bitmaps, leave, word_index - 1);
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

void leave_list_item_reset(LeaveListItem *item) {
  item->count = 0;
  item->mean = 0;
}

Rack *get_new_bag_as_rack(const LetterDistribution *ld) {
  Bag *bag = bag_create(ld);
  const int ld_size = ld_get_size(ld);
  Rack *bag_as_rack = rack_create(ld_size);

  for (int i = 0; i < ld_size; i++) {
    int number_of_tiles = bag_get_letter(bag, i);
    rack_add_letters(bag_as_rack, i, number_of_tiles);
  }

  bag_destroy(bag);
  return bag_as_rack;
}

LeaveList *leave_list_create(const LetterDistribution *ld, KLV *klv,
                             int target_min_leave_count) {
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

  leave_list->target_min_leave_count = target_min_leave_count;
  leave_list->leaves_under_target_min_count = leave_list->number_of_leaves;

  const int ld_size = ld_get_size(ld);

  leave_list->subleave = rack_create(ld_size);

  Rack *bag_as_rack = get_new_bag_as_rack(ld);

  leave_list->leave_bitmaps =
      leave_bitmaps_create(ld, leave_list->number_of_leaves);

  const int number_of_set_leaves = leave_list_set_item_leaves(
      leave_list, ld, bag_as_rack, leave_list->subleave,
      kwg_get_dawg_root_node_index(leave_list->klv->kwg), 0, 0);

  if (number_of_set_leaves != leave_list->number_of_leaves) {
    log_fatal("Did not set the correct number of leaves:\nnumber of set "
              "leaves: %d\nnumber of leaves: %d\n",
              number_of_set_leaves, leave_list->number_of_leaves);
  }

  rack_destroy(bag_as_rack);

  leave_list->prng = prng_create(0);

  return leave_list;
}

void leave_list_destroy(LeaveList *leave_list) {
  if (!leave_list) {
    return;
  }
  for (int i = 0; i < leave_list->number_of_leaves; i++) {
    free(leave_list->leaves_ordered_by_klv_index[i]);
  }
  free(leave_list->leaves_ordered_by_klv_index);
  free(leave_list->leaves_ordered_by_count);
  free(leave_list->empty_leave);
  leave_count_hashmap_destroy(leave_list->leave_count_hashmap);
  rack_destroy(leave_list->subleave);
  leave_bitmaps_destroy(leave_list->leave_bitmaps);
  prng_destroy(leave_list->prng);
  free(leave_list);
}

void leave_list_reset(LeaveList *leave_list) {
  leave_list_item_reset(leave_list->empty_leave);
  for (int i = 0; i < leave_list->number_of_leaves; i++) {
    leave_list_item_reset(leave_list->leaves_ordered_by_count[i]);
  }
  leave_count_hashmap_reset(leave_list->leave_count_hashmap);
  leave_count_hashmap_set(leave_list->leave_count_hashmap, 0,
                          leave_list->number_of_leaves - 1);

  leave_list->leaves_under_target_min_count = leave_list->number_of_leaves;
}

void leave_list_item_increment_count(LeaveListItem *item, double equity) {
  item->count++;
  item->mean += (1.0 / item->count) * (equity - item->mean);
}

void leave_list_swap_items(LeaveList *leave_list, int i, int j) {
  // Perform the swap
  LeaveListItem *temp = leave_list->leaves_ordered_by_count[i];
  leave_list->leaves_ordered_by_count[i] =
      leave_list->leaves_ordered_by_count[j];
  leave_list->leaves_ordered_by_count[j] = temp;

  // Reassign count indexes
  leave_list->leaves_ordered_by_count[i]->count_index = i;
  leave_list->leaves_ordered_by_count[j]->count_index = j;

  leave_bitmaps_swap_leaves(leave_list->leave_bitmaps, i, j);
}

int leave_list_get_lowest_leave_count(const LeaveList *leave_list) {
  return leave_list->leaves_ordered_by_count[0]->count;
}

// Returns the minimum leave count for the updated leave list.
int leave_list_add_subleave_with_klv_index(LeaveList *leave_list, int klv_index,
                                           double equity) {
  LeaveListItem *item = leave_list->leaves_ordered_by_klv_index[klv_index];

  int old_count = item->count;
  leave_list_item_increment_count(item, equity);
  int new_count = item->count;

  if (new_count == leave_list->target_min_leave_count) {
    leave_list->leaves_under_target_min_count--;
  }

  uint64_t old_count_end_index =
      leave_count_hashmap_get(leave_list->leave_count_hashmap, old_count);

  leave_list_swap_items(leave_list, old_count_end_index, item->count_index);

  if (old_count_end_index == 0 ||
      leave_list->leaves_ordered_by_count[old_count_end_index - 1]->count !=
          old_count) {
    leave_count_hashmap_delete(leave_list->leave_count_hashmap, old_count);
  } else {
    leave_count_hashmap_set(leave_list->leave_count_hashmap, old_count,
                            old_count_end_index - 1);
  }

  uint64_t new_count_end_index =
      leave_count_hashmap_get(leave_list->leave_count_hashmap, new_count);
  if (new_count_end_index == UNSET_KEY_OR_VALUE) {
    leave_count_hashmap_set(leave_list->leave_count_hashmap, new_count,
                            old_count_end_index);
    new_count_end_index = old_count_end_index;
  }

  // Swap the newly incremented item with another item of the
  // same count to randomize the order of items having the
  // same count.
  // The old_count_end_index is now the new count start index
  // which is why we +1 here.
  const uint64_t number_of_new_count_items =
      new_count_end_index - old_count_end_index + 1;
  const uint64_t random_new_count_index =
      prng_get_random_number(leave_list->prng, number_of_new_count_items) +
      old_count_end_index;

  leave_list_swap_items(leave_list, old_count_end_index,
                        (int)random_new_count_index);

  return leave_list_get_lowest_leave_count(leave_list);
}

// Adds a single subleave to the list.
// Returns the lowest leave count.
int leave_list_add_subleave(LeaveList *leave_list, const Rack *subleave,
                            double equity) {
  return leave_list_add_subleave_with_klv_index(
      leave_list, klv_get_word_index(leave_list->klv, subleave), equity);
}

void generate_subleaves(LeaveList *leave_list, uint32_t node_index,
                        uint32_t word_index, uint8_t ml) {
  const uint32_t ld_size = rack_get_dist_size(leave_list->full_rack);
  while (ml < ld_size && rack_get_letter(leave_list->full_rack, ml) == 0) {
    ml++;
  }
  if (ml == ld_size) {
    const int number_of_letters_in_subleave =
        rack_get_total_letters(leave_list->subleave);
    if (number_of_letters_in_subleave > 0) {
      // Superleaves will only contain all possible subleaves
      // of size RACK_SIZE - 1 and below.
      if (number_of_letters_in_subleave < (RACK_SIZE)) {
        leave_list_add_subleave_with_klv_index(leave_list, word_index - 1,
                                               leave_list->move_equity);
      }
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

// Adds a leave and all of its subleaves to the list. So for a
// leave of X letters where X < RACK_SIZE, 2^X subleaves will be added.
// Returns the minimum count of the leaves in the list after adding
// all the subleaves.
int leave_list_add_leave(LeaveList *leave_list, Rack *full_rack,
                         double move_equity) {
  leave_list->full_rack = full_rack;
  leave_list->move_equity = move_equity;
  rack_reset(leave_list->subleave);
  generate_subleaves(leave_list,
                     kwg_get_dawg_root_node_index(leave_list->klv->kwg), 0, 0);
  return leave_list_get_lowest_leave_count(leave_list);
}

void leave_list_write_to_klv(LeaveList *leave_list) {
  int number_of_leaves = leave_list->number_of_leaves;
  LeaveListItem **items = leave_list->leaves_ordered_by_klv_index;
  double average_leave_value = leave_list->empty_leave->mean;
  KLV *klv = leave_list->klv;
  for (int i = 0; i < number_of_leaves; i++) {
    if (items[i]->count > 0) {
      klv->leave_values[i] = items[i]->mean - average_leave_value;
    }
  }
}

bool leave_list_draw_rarest_available_leave(LeaveList *leave_list, Bag *bag,
                                            Rack *rack, Rack *empty_rack,
                                            int player_draw_index) {
  return leave_bitmaps_draw_first_available_subrack(
      leave_list->leave_bitmaps, bag, rack, empty_rack, player_draw_index,
      leave_list->leaves_under_target_min_count);
}

int leave_list_get_target_min_leave_count(const LeaveList *leave_list) {
  return leave_list->target_min_leave_count;
}

int leave_list_get_leaves_under_target_min_count(const LeaveList *leave_list) {
  return leave_list->leaves_under_target_min_count;
}

int leave_list_get_number_of_leaves(const LeaveList *leave_list) {
  return leave_list->number_of_leaves;
}

uint64_t leave_list_get_count(const LeaveList *leave_list, int count_index) {
  return leave_list->leaves_ordered_by_count[count_index]->count;
}

double leave_list_get_mean(const LeaveList *leave_list, int count_index) {
  return leave_list->leaves_ordered_by_count[count_index]->mean;
}

int leave_list_get_empty_leave_count(const LeaveList *leave_list) {
  return leave_list->empty_leave->count;
}

double leave_list_get_empty_leave_mean(const LeaveList *leave_list) {
  return leave_list->empty_leave->mean;
}

int leave_list_get_count_index(const LeaveList *leave_list, int klv_index) {
  return leave_list->leaves_ordered_by_klv_index[klv_index]->count_index;
}

// Calls thread_control_copy_to_dst_and_jump with the leave list
// PRNG as the dst PRNG.
void leave_list_seed(LeaveList *leave_list, ThreadControl *thread_control) {
  thread_control_copy_to_dst_and_jump(thread_control, leave_list->prng);
}

void string_builder_add_most_or_least_common_leaves(
    StringBuilder *sb, const LeaveList *leave_list,
    const LetterDistribution *ld, int n, bool most_common) {
  const int number_of_leaves = leave_list->number_of_leaves;
  int i = 0;
  if (most_common) {
    i = number_of_leaves - 1;
    string_builder_add_formatted_string(sb, "Top %d most common leaves:\n\n",
                                        n);
  } else {
    string_builder_add_formatted_string(sb, "Top %d least common leaves:\n\n",
                                        n);
  }
  while (i < number_of_leaves && i >= 0 && n > 0) {
    const int count = leave_list->leaves_ordered_by_count[i]->count;
    // Here we use the subleave to help print the most/leave common leaves
    Rack *rack = leave_list->subleave;
    rack_reset(rack);
    leave_bitmaps_draw_to_leave(leave_list->leave_bitmaps, NULL, rack, 0, i);
    string_builder_add_rack(sb, rack, ld, false);
    string_builder_add_formatted_string(
        sb, "%*s %d\n", (RACK_SIZE)-rack_get_total_letters(rack), "", count);
    n--;
    if (most_common) {
      i--;
    } else {
      i++;
    }
  }
}