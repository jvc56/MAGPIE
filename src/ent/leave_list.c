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
  int count;
  double mean;
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
  LeaveListItem *empty_leave;
  // Ordered by klv index
  LeaveListItem **leaves;
};

LeaveListItem *leave_list_item_create(void) {
  LeaveListItem *item = malloc_or_die(sizeof(LeaveListItem));
  item->count = 0;
  item->mean = 0;
  return item;
}

void leave_list_item_reset(LeaveListItem *item) {
  item->count = 0;
  item->mean = 0;
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
  leave_list->leaves_under_target_min_count = leave_list->number_of_leaves;
  leave_list->rack = rack_create(ld_get_size(ld));
  leave_list->bag_as_rack = rack_create(ld_get_size(ld));
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
}

void leave_list_item_increment_count(LeaveListItem *item, double equity) {
  item->count++;
  item->mean += (1.0 / item->count) * (equity - item->mean);
}

// Returns the minimum leave count for the updated leave list.
int leave_list_add_subleave_with_klv_index(LeaveList *leave_list, int klv_index,
                                           double equity) {
  LeaveListItem *item = leave_list->leaves[klv_index];
  leave_list_item_increment_count(item, equity);
  if (item->count == leave_list->target_min_leave_count) {
    leave_list->leaves_under_target_min_count--;
  }
  return -1;
}

// Adds a single rack to the list.
// Returns the lowest leave count.
int leave_list_add_subleave(LeaveList *leave_list, const Rack *rack,
                            double equity) {
  return leave_list_add_subleave_with_klv_index(
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
        leave_list_add_subleave_with_klv_index(leave_list, word_index - 1,
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

// Adds a leave and all of its subleaves to the list. So for a
// leave of X letters where X < RACK_SIZE, 2^X subleaves will be added.
// Returns the minimum count of the leaves in the list after adding
// all the subleaves.
// FIXME: needs a better name
int leave_list_add_leave(LeaveList *leave_list, Rack *player_rack,
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
  const int number_of_letters_in_leave =
      rack_get_total_letters(leave_list->rare_leave);
  if (ml == ld_size && number_of_letters_in_leave <= (RACK_SIZE)) {
    if (number_of_letters_in_leave > 0) {
      if (word_index == KLV_UNFOUND_INDEX) {
        log_fatal("word index not found in klv: %u", word_index);
      }
      if (leave_list->leaves[word_index - 1]->count <
          leave_list->target_min_leave_count) {
        return true;
      }
    }
  } else {
    // FIXME: use better name
    int num_ml_already_on_player_rack =
        rack_get_letter(leave_list->player_rack, ml);
    tiles_on_rack += num_ml_already_on_player_rack;
    if (tiles_on_rack > (RACK_SIZE)) {
      return false;
    }

    if (leave_list_draw_rare_leave_internal(leave_list, ld, player_draw_index,
                                            node_index, word_index, ml + 1,
                                            tiles_on_rack)) {
      return true;
    }
    int max_num_bag_ml_to_add = rack_get_letter(leave_list->bag_as_rack, ml);
    if (number_of_letters_in_leave + max_num_bag_ml_to_add > MAX_LEAVE_SIZE) {
      max_num_bag_ml_to_add = MAX_LEAVE_SIZE - number_of_letters_in_leave;
    }
    // FIXME: use better name
    int num_added = 0;
    // FIXME: don't add over rack size
    for (int i = 0; i < max_num_bag_ml_to_add; i++) {
      rack_add_letter(leave_list->rare_leave, ml);
      rack_take_letter(leave_list->bag_as_rack, ml);
      num_added++;
      uint32_t sibling_word_index;
      node_index = increment_node_to_ml(leave_list->klv, node_index, word_index,
                                        &sibling_word_index, ml);
      word_index = sibling_word_index;
      uint32_t child_word_index;
      node_index = follow_arc(leave_list->klv, node_index, word_index,
                              &child_word_index);
      word_index = child_word_index;
      int additional_num_required_tiles =
          num_added - num_ml_already_on_player_rack;
      if (additional_num_required_tiles < 0) {
        additional_num_required_tiles = 0;
      }
      if (leave_list_draw_rare_leave_internal(
              leave_list, ld, player_draw_index, node_index, word_index, ml + 1,
              tiles_on_rack + additional_num_required_tiles)) {
        return true;
      }
    }
    rack_take_letters(leave_list->rare_leave, ml, max_num_bag_ml_to_add);
    rack_add_letters(leave_list->bag_as_rack, ml, max_num_bag_ml_to_add);
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
      kwg_get_dawg_root_node_index(leave_list->klv->kwg), 0, 0, 0);
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

uint64_t leave_list_get_count(const LeaveList *leave_list, int count_index) {
  return leave_list->leaves[count_index]->count;
}

int leave_list_get_lowest_leave_count(const LeaveList *leave_list) {
  // FIXME: implement this
  return leave_list->leaves[0]->count;
}

double leave_list_get_mean(const LeaveList *leave_list, int count_index) {
  return leave_list->leaves[count_index]->mean;
}

int leave_list_get_empty_leave_count(const LeaveList *leave_list) {
  return leave_list->empty_leave->count;
}

double leave_list_get_empty_leave_mean(const LeaveList *leave_list) {
  return leave_list->empty_leave->mean;
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
    const int count = leave_list->leaves[i]->count;
    // Here we use the rack to help print the most/leave common leaves
    Rack *rack = leave_list->rack;
    // FIXME: figure out how to print
    rack_set_to_string(ld, rack, "ABCDEFG");
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