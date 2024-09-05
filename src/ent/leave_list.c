#include "leave_list.h"

#include "bag.h"
#include "game.h"
#include "klv.h"
#include "rack.h"
#include "thread_control.h"
#include "xoshiro.h"

#include "../str/rack_string.h"

#include "../util/util.h"

#define MAX_LEAVE_SIZE (RACK_SIZE - 1)

typedef struct LeaveListItem {
  int count;
  double mean;
  int children_under_target_count;
} LeaveListItem;

struct LeaveList {
  // Owned by the caller
  KLV *klv;
  Rack *player_rack;
  Rack *rare_leave;
  // Owned by this struct
  Rack *bag_as_rack;
  Rack *rack;
  double move_equity;
  int number_of_leaves;
  // Leaves with this count are no longer considered
  // rare and are excluded from forced draws.
  int target_min_leave_count;
  int leaves_under_target_min_count;
  int *leave_counts;
  int lowest_leave_count;
  LeaveListItem *empty_leave;
  // Ordered by klv index
  LeaveListItem **leaves;
};

void leave_list_item_reset(LeaveListItem *item) {
  item->count = 0;
  item->mean = 0;
  item->children_under_target_count = 0;
}

LeaveListItem *leave_list_item_create(void) {
  LeaveListItem *item = malloc_or_die(sizeof(LeaveListItem));
  leave_list_item_reset(item);
  return item;
}

void update_children_under_target_count(LeaveList *leave_list,
                                        uint32_t node_index,
                                        uint32_t word_index, uint8_t ml,
                                        int inc_val) {
  const int ld_size = rack_get_dist_size(leave_list->rack);
  while (ml < ld_size && rack_get_letter(leave_list->rack, ml) == 0) {
    ml++;
  }

  // Use bag_as_rack to keep track of the current
  // tiles in each child leave.
  const int rack_num_letters = rack_get_total_letters(leave_list->bag_as_rack);
  if (ml == ld_size) {
    if (rack_num_letters > 0) {
      leave_list->leaves[word_index - 1]->children_under_target_count +=
          inc_val;
    }
    return;
  }

  // Add none of the current ml
  update_children_under_target_count(leave_list, node_index, word_index, ml + 1,
                                     inc_val);

  const int num_ml_to_add = rack_get_letter(leave_list->rack, ml);
  for (int i = 0; i < num_ml_to_add; i++) {
    rack_add_letter(leave_list->bag_as_rack, ml);
    uint32_t sibling_word_index;
    node_index = increment_node_to_ml(leave_list->klv, node_index, word_index,
                                      &sibling_word_index, ml);
    word_index = sibling_word_index;
    uint32_t child_word_index;
    node_index =
        follow_arc(leave_list->klv, node_index, word_index, &child_word_index);
    word_index = child_word_index;
    update_children_under_target_count(leave_list, node_index, word_index,
                                       ml + 1, inc_val);
  }
  rack_take_letters(leave_list->bag_as_rack, ml, num_ml_to_add);
}

void leave_list_increment_children_under_target_count(LeaveList *leave_list) {
  rack_reset(leave_list->bag_as_rack);
  update_children_under_target_count(
      leave_list, kwg_get_dawg_root_node_index(leave_list->klv->kwg), 0, 0, 1);
}

void leave_list_decrement_children_under_target_count(LeaveList *leave_list) {
  rack_reset(leave_list->bag_as_rack);
  update_children_under_target_count(
      leave_list, kwg_get_dawg_root_node_index(leave_list->klv->kwg), 0, 0, -1);
}

void set_initial_children_under_target_counts(LeaveList *leave_list,
                                              const LetterDistribution *ld,
                                              uint8_t ml) {
  const int rack_num_letters = rack_get_total_letters(leave_list->rack);
  if (ml == ld_get_size(ld)) {
    if (rack_num_letters > 0) {
      leave_list_increment_children_under_target_count(leave_list);
    }
    return;
  }

  // Add none of the current ml
  set_initial_children_under_target_counts(leave_list, ld, ml + 1);

  // Add the current ml
  int max_ml_to_add = ((RACK_SIZE)-1) - rack_num_letters;
  const int ml_dist = ld_get_dist(ld, ml);
  if (ml_dist < max_ml_to_add) {
    max_ml_to_add = ml_dist;
  }
  for (int i = 0; i < max_ml_to_add; i++) {
    rack_add_letter(leave_list->rack, ml);
    set_initial_children_under_target_counts(leave_list, ld, ml + 1);
  }
  rack_take_letters(leave_list->rack, ml, max_ml_to_add);
}

LeaveList *leave_list_create(const LetterDistribution *ld, KLV *klv,
                             int target_min_leave_count) {
  LeaveList *leave_list = malloc_or_die(sizeof(LeaveList));
  leave_list->klv = klv;
  leave_list->number_of_leaves = klv_get_number_of_leaves(klv);
  size_t leaves_malloc_size =
      sizeof(LeaveListItem *) * leave_list->number_of_leaves;
  leave_list->leaves = malloc_or_die(leaves_malloc_size);
  for (int i = 0; i < leave_list->number_of_leaves; i++) {
    leave_list->leaves[i] = leave_list_item_create();
  }
  leave_list->empty_leave = leave_list_item_create();
  leave_list->target_min_leave_count = target_min_leave_count;
  leave_list->leave_counts =
      calloc_or_die(leave_list->target_min_leave_count, sizeof(int));
  leave_list->leave_counts[0] = leave_list->number_of_leaves;
  leave_list->lowest_leave_count = 0;
  leave_list->leaves_under_target_min_count = leave_list->number_of_leaves;
  leave_list->rack = rack_create(ld_get_size(ld));
  leave_list->bag_as_rack = rack_create(ld_get_size(ld));

  set_initial_children_under_target_counts(leave_list, ld, 0);

  return leave_list;
}

void leave_list_destroy(LeaveList *leave_list) {
  if (!leave_list) {
    return;
  }
  for (int i = 0; i < leave_list->number_of_leaves; i++) {
    free(leave_list->leaves[i]);
  }
  free(leave_list->leaves);
  free(leave_list->empty_leave);
  free(leave_list->leave_counts);
  rack_destroy(leave_list->rack);
  rack_destroy(leave_list->bag_as_rack);
  free(leave_list);
}

void leave_list_reset(LeaveList *leave_list) {
  leave_list_item_reset(leave_list->empty_leave);
  for (int i = 0; i < leave_list->number_of_leaves; i++) {
    leave_list_item_reset(leave_list->leaves[i]);
  }
  leave_list->leaves_under_target_min_count = leave_list->number_of_leaves;
  memset(leave_list->leave_counts, 0,
         sizeof(int) * leave_list->target_min_leave_count);
  leave_list->leave_counts[0] = leave_list->number_of_leaves;
  leave_list->lowest_leave_count = 0;
}

void leave_list_item_increment_count(LeaveListItem *item, double equity) {
  item->count++;
  item->mean += (1.0 / item->count) * (equity - item->mean);
}

int leave_list_get_lowest_leave_count(const LeaveList *leave_list) {
  return leave_list->lowest_leave_count;
}

// Returns the lowest leave count for the updated leave list.
int leave_list_add_single_leave_with_klv_index(LeaveList *leave_list,
                                               int klv_index, double equity) {
  LeaveListItem *item = leave_list->leaves[klv_index];
  leave_list_item_increment_count(item, equity);
  const int new_count = item->count;
  if (new_count <= leave_list->target_min_leave_count) {
    // Update the number of leaves under the target minimum
    if (new_count == leave_list->target_min_leave_count) {
      leave_list->leaves_under_target_min_count--;
    }
    // Update the leave counts
    leave_list->leave_counts[new_count - 1]--;
    if (new_count < leave_list->target_min_leave_count) {
      leave_list->leave_counts[new_count]++;
    }
    // Update the lowest leave count
    if (leave_list->leave_counts[new_count - 1] == 0 &&
        leave_list->lowest_leave_count == new_count - 1) {
      leave_list->lowest_leave_count++;
    }
  }
  return leave_list_get_lowest_leave_count(leave_list);
}

// Adds a single rack to the list.
// Returns the lowest leave count.
int leave_list_add_single_leave(LeaveList *leave_list, const Rack *rack,
                                double equity) {
  return leave_list_add_single_leave_with_klv_index(
      leave_list, klv_get_word_index(leave_list->klv, rack), equity);
}

void generate_subleaves(LeaveList *leave_list, uint32_t node_index,
                        uint32_t word_index, uint8_t ml) {
  const uint32_t ld_size = rack_get_dist_size(leave_list->player_rack);
  while (ml < ld_size && rack_get_letter(leave_list->player_rack, ml) == 0) {
    ml++;
  }
  if (ml == ld_size) {
    const int number_of_letters_in_subleave =
        rack_get_total_letters(leave_list->rack);
    if (number_of_letters_in_subleave > 0) {
      // Superleaves will only contain all possible subleaves
      // of size RACK_SIZE - 1 and below.
      if (number_of_letters_in_subleave < (RACK_SIZE)) {
        leave_list_add_single_leave_with_klv_index(leave_list, word_index - 1,
                                                   leave_list->move_equity);
      }
    } else {
      leave_list_item_increment_count(leave_list->empty_leave,
                                      leave_list->move_equity);
    }
  } else {
    generate_subleaves(leave_list, node_index, word_index, ml + 1);
    const int num_this = rack_get_letter(leave_list->player_rack, ml);
    for (int i = 0; i < num_this; i++) {
      rack_add_letter(leave_list->rack, ml);
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
    rack_take_letters(leave_list->rack, ml, num_this);
  }
}

// Adds every subset of tiles on the rack as a leave with equity move_equity. So
// for a leave of X distinct letters, 2^X - 1 subleaves will be added. The -1
// is because the full rack is not added as a leave.
// Returns the minimum count of the leaves in the list after adding
// all the subleaves.
int leave_list_add_leaves_for_rack(LeaveList *leave_list, Rack *player_rack,
                                   double move_equity) {
  leave_list->player_rack = player_rack;
  leave_list->move_equity = move_equity;
  rack_reset(leave_list->rack);
  generate_subleaves(leave_list,
                     kwg_get_dawg_root_node_index(leave_list->klv->kwg), 0, 0);
  return leave_list_get_lowest_leave_count(leave_list);
}

void leave_list_write_to_klv(LeaveList *leave_list) {
  int number_of_leaves = leave_list->number_of_leaves;
  LeaveListItem **items = leave_list->leaves;
  double average_leave_value = leave_list->empty_leave->mean;
  KLV *klv = leave_list->klv;
  for (int i = 0; i < number_of_leaves; i++) {
    if (items[i]->count > 0) {
      klv->leave_values[i] = items[i]->mean - average_leave_value;
    }
  }
}

bool leave_list_draw_rare_leave_internal(
    LeaveList *leave_list, const LetterDistribution *ld, int player_draw_index,
    uint32_t node_index, uint32_t word_index, uint8_t ml, int tiles_on_rack) {
  const uint32_t ld_size = ld_get_size(ld);
  while (ml < ld_size && rack_get_letter(leave_list->bag_as_rack, ml) == 0) {
    ml++;
  }

  const int num_letters_in_leave =
      rack_get_total_letters(leave_list->rare_leave);
  if (ml == ld_size) {
    if (num_letters_in_leave > 0) {
      if (word_index == KLV_UNFOUND_INDEX) {
        log_fatal("word index not found in klv: %u", word_index);
      }
      if (leave_list->leaves[word_index - 1]->count <
          leave_list->target_min_leave_count) {
        return true;
      }
    }
  } else {
    // Advance to the next machine letter without adding any of the current
    // machine letter.
    if (leave_list_draw_rare_leave_internal(leave_list, ld, player_draw_index,
                                            node_index, word_index, ml + 1,
                                            tiles_on_rack)) {
      return true;
    }

    // Given the current rack and bag, the maximum number of current
    // machine letters we can add to the current leave is limited by the
    // following constraints:
    //
    //   1. The total number of tiles on the rack must be less than or equal to
    //      RACK_SIZE. Using this constraint, the number of addition tiles we
    //      can add to the current leave is limited to num_ml_in_rack +
    //      (RACK_SIZE)-tiles_on_rack. We can add num_ml_in_rack to this limit
    //      since those tiles are already part of the rack, and do not increase
    //      the total rack size.
    //
    //   2. The total number of tiles on the leave must be less than or equal to
    //      RACK_SIZE - 1. Using this constraint, the number of addition tiles
    //      we can add to the current leave is limited to ((RACK_SIZE)-1) -
    //      num_letters_in_leave. This is just the remaining number of available
    //      spaces for tiles in the leave.
    //
    //   3. The total number of current machine letter must be less
    //      than or equal to the number of that machine letter available in the
    //      bag_as_rack, which is the combined player rack + bag.
    //
    // We take the minimum of these values to determine the maximum number of
    // tiles we can add to the current leave.

    // Save the number of the current machine letter for convenience.
    const int num_ml_in_rack = rack_get_letter(leave_list->player_rack, ml);

    // Establish the maximums described above.
    const int rack_size_max = num_ml_in_rack + ((RACK_SIZE)-tiles_on_rack);
    const int leave_size_max = ((RACK_SIZE)-1) - num_letters_in_leave;
    const int bag_ml_max = rack_get_letter(leave_list->bag_as_rack, ml);

    // Get the minimum of the maximums.
    int max_ml_to_add = rack_size_max;
    if (max_ml_to_add > leave_size_max) {
      max_ml_to_add = leave_size_max;
    }
    if (max_ml_to_add > bag_ml_max) {
      max_ml_to_add = bag_ml_max;
    }

    for (int ml_added = 1; ml_added <= max_ml_to_add; ml_added++) {
      rack_add_letter(leave_list->rare_leave, ml);
      rack_take_letter(leave_list->bag_as_rack, ml);
      uint32_t sibling_word_index;
      node_index = increment_node_to_ml(leave_list->klv, node_index, word_index,
                                        &sibling_word_index, ml);
      word_index = sibling_word_index;
      uint32_t child_word_index;
      node_index = follow_arc(leave_list->klv, node_index, word_index,
                              &child_word_index);
      word_index = child_word_index;

      // If the number of machine letters added to the leave exceeds the number
      // of machine letters already on the rack, the rack size will increase.
      int tiles_added_to_rack = 0;
      if (ml_added > num_ml_in_rack) {
        tiles_added_to_rack = ml_added - num_ml_in_rack;
      }
      if (leave_list_draw_rare_leave_internal(
              leave_list, ld, player_draw_index, node_index, word_index, ml + 1,
              tiles_on_rack + tiles_added_to_rack)) {
        return true;
      }
    }
    rack_take_letters(leave_list->rare_leave, ml, max_ml_to_add);
    rack_add_letters(leave_list->bag_as_rack, ml, max_ml_to_add);
  }
  return false;
}

// Draws the minimum number of tiles necessary to make the rack_to_draw
// a subset of the player_rack.
// Assumes the rack is a subset of the rack_to_draw.
// Assumes the bag has the required number of tiles.
void draw_to_rack(Bag *bag, Rack *player_rack, const Rack *rack_to_draw,
                  int player_draw_index) {
  const int dist_size = rack_get_dist_size(player_rack);
  for (int i = 0; i < dist_size; i++) {
    const int num_to_draw =
        rack_get_letter(rack_to_draw, i) - rack_get_letter(player_rack, i);
    if (num_to_draw > 0) {
      if (!bag_draw_letters(bag, i, num_to_draw, player_draw_index)) {
        log_fatal("attempted to draw letter %d from bag, but failed\n", i);
      }
      rack_add_letters(player_rack, i, num_to_draw);
    }
  }
}

void copy_bag_and_player_rack_to_rack(Rack *rack, const Bag *bag,
                                      const Rack *player_rack) {
  int dist_size = rack_get_dist_size(rack);
  rack_reset(rack);
  for (int i = 0; i < dist_size; i++) {
    rack_add_letters(rack, i,
                     bag_get_letter(bag, i) + rack_get_letter(player_rack, i));
  }
}

bool leave_list_draw_rare_leave(LeaveList *leave_list,
                                const LetterDistribution *ld, Bag *bag,
                                Rack *player_rack, int player_draw_index,
                                Rack *rare_leave) {
  copy_bag_and_player_rack_to_rack(leave_list->bag_as_rack, bag, player_rack);
  rack_reset(rare_leave);
  leave_list->player_rack = player_rack;
  leave_list->rare_leave = rare_leave;
  bool success = leave_list_draw_rare_leave_internal(
      leave_list, ld, player_draw_index,
      kwg_get_dawg_root_node_index(leave_list->klv->kwg), 0, 0,
      rack_get_total_letters(leave_list->player_rack));
  if (success) {
    draw_to_rack(bag, player_rack, leave_list->rare_leave, player_draw_index);
  }
  return success;
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

uint64_t leave_list_get_count(const LeaveList *leave_list, int klv_index) {
  return leave_list->leaves[klv_index]->count;
}

double leave_list_get_mean(const LeaveList *leave_list, int klv_index) {
  return leave_list->leaves[klv_index]->mean;
}

int leave_list_get_empty_leave_count(const LeaveList *leave_list) {
  return leave_list->empty_leave->count;
}

double leave_list_get_empty_leave_mean(const LeaveList *leave_list) {
  return leave_list->empty_leave->mean;
}

int leave_list_get_children_under_target_count(const LeaveList *leave_list,
                                               int klv_index) {
  return leave_list->leaves[klv_index]->children_under_target_count;
}