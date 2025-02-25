#include "leave_list.h"

#include <pthread.h>

#include "bag.h"
#include "encoded_rack.h"
#include "game.h"
#include "klv.h"
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
  EncodedRack encoded_rack;
  pthread_mutex_t mutex;
} LeaveListItem;

struct LeaveList {
  int number_of_leaves;
  // Leaves with this count are no longer considered
  // rare and are excluded from forced draws.
  int target_leave_count;
  // Index of the last leave in the leaves_partitioned_by_target_count
  // which has a count less than target_leave_count. All leaves
  // with an index greater than this are no longer considered rare.
  // All leaves with an index less than or equal to this are considered rare.
  int partition_index;
  int number_of_threads;
  uint64_t *empty_leave_counts;
  double *empty_leave_means;
  // Owned by the caller
  KLV *klv;
  LeaveListItem **leaves_ordered_by_klv_index;
  LeaveListItem **leaves_partitioned_by_target_count;
  pthread_mutex_t partition_index_mutex;
};

LeaveListItem *leave_list_item_create(int count_index) {
  LeaveListItem *item = malloc_or_die(sizeof(LeaveListItem));
  item->count = 0;
  item->mean = 0;
  item->count_index = count_index;
  pthread_mutex_init(&item->mutex, NULL);
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
      rack_encode(leave,
                  &leave_list->leaves_ordered_by_klv_index[word_index - 1]
                       ->encoded_rack);
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

void leave_list_reset_empty_leave(LeaveList *leave_list) {
  for (int i = 0; i < leave_list->number_of_threads; i++) {
    leave_list->empty_leave_counts[i] = 0;
    leave_list->empty_leave_means[i] = 0;
  }
}

LeaveList *leave_list_create(const LetterDistribution *ld, KLV *klv,
                             int target_leave_count, int number_of_threads) {
  LeaveList *leave_list = malloc_or_die(sizeof(LeaveList));
  leave_list->klv = klv;

  leave_list->number_of_leaves = klv_get_number_of_leaves(klv);
  size_t leaves_malloc_size =
      sizeof(LeaveListItem *) * leave_list->number_of_leaves;
  leave_list->leaves_ordered_by_klv_index = malloc_or_die(leaves_malloc_size);
  for (int i = 0; i < leave_list->number_of_leaves; i++) {
    leave_list->leaves_ordered_by_klv_index[i] = leave_list_item_create(i);
  }

  leave_list->number_of_threads = number_of_threads;
  leave_list->empty_leave_counts =
      malloc_or_die(sizeof(uint64_t) * leave_list->number_of_threads);
  leave_list->empty_leave_means =
      malloc_or_die(sizeof(double) * leave_list->number_of_threads);
  leave_list_reset_empty_leave(leave_list);

  leave_list->leaves_partitioned_by_target_count =
      malloc_or_die(leaves_malloc_size);

  memory_copy(leave_list->leaves_partitioned_by_target_count,
              leave_list->leaves_ordered_by_klv_index, leaves_malloc_size);

  leave_list->partition_index = leave_list->number_of_leaves - 1;
  pthread_mutex_init(&leave_list->partition_index_mutex, NULL);
  leave_list->target_leave_count = target_leave_count;

  const int ld_size = ld_get_size(ld);

  Rack *bag_as_rack = get_new_bag_as_rack(ld);
  Rack *subleave = rack_create(ld_size);

  const int number_of_set_leaves = leave_list_set_item_leaves(
      leave_list, ld, bag_as_rack, subleave,
      kwg_get_dawg_root_node_index(leave_list->klv->kwg), 0, 0);

  if (number_of_set_leaves != leave_list->number_of_leaves) {
    log_fatal("Did not set the correct number of leaves:\nnumber of set "
              "leaves: %d\nnumber of leaves: %d\n",
              number_of_set_leaves, leave_list->number_of_leaves);
  }

  rack_destroy(subleave);
  rack_destroy(bag_as_rack);

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
  free(leave_list->leaves_partitioned_by_target_count);
  free(leave_list->empty_leave_counts);
  free(leave_list->empty_leave_means);
  free(leave_list);
}

void leave_list_reset(LeaveList *leave_list, int target_leave_count) {
  leave_list_reset_empty_leave(leave_list);
  for (int i = 0; i < leave_list->number_of_leaves; i++) {
    leave_list_item_reset(leave_list->leaves_partitioned_by_target_count[i]);
  }
  leave_list->partition_index = leave_list->number_of_leaves - 1;
  leave_list->target_leave_count = target_leave_count;
}

void leave_list_item_increment_count(LeaveListItem *item, double equity) {
  item->count++;
  item->mean += (1.0 / item->count) * (equity - item->mean);
}

void leave_list_item_increment_empty_leave_count(LeaveList *leave_list,
                                                 int thread_index,
                                                 double equity) {
  leave_list->empty_leave_counts[thread_index]++;
  const int count = leave_list->empty_leave_counts[thread_index];
  const double old_mean = leave_list->empty_leave_means[thread_index];
  leave_list->empty_leave_means[thread_index] +=
      (1.0 / count) * (equity - old_mean);
}

void leave_list_swap_items(LeaveList *leave_list, int i, int j) {
  // Perform the swap
  LeaveListItem *temp = leave_list->leaves_partitioned_by_target_count[i];
  leave_list->leaves_partitioned_by_target_count[i] =
      leave_list->leaves_partitioned_by_target_count[j];
  leave_list->leaves_partitioned_by_target_count[j] = temp;

  // Reassign count indexes
  leave_list->leaves_partitioned_by_target_count[i]->count_index = i;
  leave_list->leaves_partitioned_by_target_count[j]->count_index = j;
}

void leave_list_add_subleave_with_klv_index(LeaveList *leave_list,
                                            int klv_index, double equity) {
  LeaveListItem *item = leave_list->leaves_ordered_by_klv_index[klv_index];
  bool item_reached_target_count = false;
  pthread_mutex_lock(&item->mutex);
  leave_list_item_increment_count(item, equity);
  item_reached_target_count = item->count == leave_list->target_leave_count;
  pthread_mutex_unlock(&item->mutex);

  if (item_reached_target_count) {
    pthread_mutex_lock(&leave_list->partition_index_mutex);
    const int curr_partition_index = leave_list->partition_index;
    if (curr_partition_index >= item->count_index) {
      if (curr_partition_index != item->count_index) {
        LeaveListItem *swap_item =
            leave_list
                ->leaves_partitioned_by_target_count[curr_partition_index];
        pthread_mutex_lock(&item->mutex);
        pthread_mutex_lock(&swap_item->mutex);
        leave_list_swap_items(leave_list, item->count_index,
                              swap_item->count_index);
        pthread_mutex_unlock(&swap_item->mutex);
        pthread_mutex_unlock(&item->mutex);
      }
      leave_list->partition_index--;
    }
    pthread_mutex_unlock(&leave_list->partition_index_mutex);
  }
}

// Adds a single subleave to the list.
void leave_list_add_single_subleave(LeaveList *leave_list, int thread_index,
                                    const Rack *subleave, double equity) {
  if (rack_is_empty(subleave)) {
    leave_list_item_increment_empty_leave_count(leave_list, thread_index,
                                                equity);
  } else {
    leave_list_add_subleave_with_klv_index(
        leave_list, klv_get_word_index(leave_list->klv, subleave), equity);
  }
}

void generate_subleaves(LeaveList *leave_list, int thread_index,
                        Rack *full_rack, Rack *subleave, double move_equity,
                        uint32_t node_index, uint32_t word_index, uint8_t ml) {
  const uint16_t ld_size = rack_get_dist_size(full_rack);
  while (ml < ld_size && rack_get_letter(full_rack, ml) == 0) {
    ml++;
  }
  if (ml == ld_size) {
    const int number_of_letters_in_subleave = rack_get_total_letters(subleave);
    if (number_of_letters_in_subleave > 0) {
      // Superleaves will only contain all possible subleaves
      // of size RACK_SIZE - 1 and below.
      if (number_of_letters_in_subleave < (RACK_SIZE)) {
        leave_list_add_subleave_with_klv_index(leave_list, word_index - 1,
                                               move_equity);
      }
    } else {
      leave_list_item_increment_empty_leave_count(leave_list, thread_index,
                                                  move_equity);
    }
  } else {
    generate_subleaves(leave_list, thread_index, full_rack, subleave,
                       move_equity, node_index, word_index, ml + 1);
    const int num_this = rack_get_letter(full_rack, ml);
    for (int i = 0; i < num_this; i++) {
      rack_add_letter(subleave, ml);
      rack_take_letter(full_rack, ml);
      uint32_t sibling_word_index;
      node_index = increment_node_to_ml(leave_list->klv, node_index, word_index,
                                        &sibling_word_index, ml);
      word_index = sibling_word_index;
      uint32_t child_word_index;
      node_index = follow_arc(leave_list->klv, node_index, word_index,
                              &child_word_index);
      word_index = child_word_index;
      generate_subleaves(leave_list, thread_index, full_rack, subleave,
                         move_equity, node_index, word_index, ml + 1);
    }

    rack_take_letters(subleave, ml, num_this);
    rack_add_letters(full_rack, ml, num_this);
  }
}

// Adds a leave and all of its subleaves to the list. So for a
// leave of X letters where X < RACK_SIZE, 2^X subleaves will be added.
// Resets the subleave rack.
void leave_list_add_all_subleaves(LeaveList *leave_list, int thread_index,
                                  Rack *full_rack, Rack *subleave,
                                  double move_equity) {
  rack_reset(subleave);
  generate_subleaves(leave_list, thread_index, full_rack, subleave, move_equity,
                     kwg_get_dawg_root_node_index(leave_list->klv->kwg), 0, 0);
}

void leave_list_write_to_klv(LeaveList *leave_list) {
  int number_of_leaves = leave_list->number_of_leaves;
  LeaveListItem **items = leave_list->leaves_ordered_by_klv_index;
  double average_leave_value = leave_list_get_empty_leave_mean(leave_list);
  KLV *klv = leave_list->klv;
  for (int i = 0; i < number_of_leaves; i++) {
    if (items[i]->count > 0) {
      klv->leave_values[i] =
          double_to_equity(items[i]->mean - average_leave_value);
    }
  }
}

int leave_list_get_leaves_below_target_count(const LeaveList *leave_list) {
  return leave_list->partition_index + 1;
}

// Returns false if there are no rare leaves remaining in the list.
bool leave_list_get_rare_leave(LeaveList *leave_list, XoshiroPRNG *prng,
                               Rack *rack) {
  // Since we don't lock the partition index, it's possible that the
  // selected random rack will have already reached the minimum target
  // leave count and no longer be rare, but that's okay since we
  // can still use the rack and most of the time the rack will still
  // be rare. Not having to lock every time we need a rare rack is worth
  // occasionally incrementing a rack that is no longer rare due to race
  // conditions.
  if (leave_list_get_leaves_below_target_count(leave_list) == 0) {
    return false;
  }
  const int random_rack_index =
      prng_get_random_number(prng, leave_list->partition_index + 1);
  rack_decode(&leave_list->leaves_partitioned_by_target_count[random_rack_index]
                   ->encoded_rack,
              rack);
  return true;
}

int leave_list_get_target_leave_count(const LeaveList *leave_list) {
  return leave_list->target_leave_count;
}

int leave_list_get_number_of_leaves(const LeaveList *leave_list) {
  return leave_list->number_of_leaves;
}

uint64_t leave_list_get_count(const LeaveList *leave_list, int klv_index) {
  return leave_list->leaves_ordered_by_klv_index[klv_index]->count;
}

double leave_list_get_mean(const LeaveList *leave_list, int klv_index) {
  return leave_list->leaves_ordered_by_klv_index[klv_index]->mean;
}

uint64_t leave_list_get_empty_leave_count(const LeaveList *leave_list) {
  uint64_t total_count = 0;
  for (int i = 0; i < leave_list->number_of_threads; i++) {
    total_count += leave_list->empty_leave_counts[i];
  }
  return total_count;
}

double leave_list_get_empty_leave_mean(const LeaveList *leave_list) {
  double mean = 0.0;
  uint64_t total_count = leave_list_get_empty_leave_count(leave_list);
  if (total_count == 0) {
    return 0.0;
  }
  for (int i = 0; i < leave_list->number_of_threads; i++) {
    mean += leave_list->empty_leave_means[i] *
            ((double)leave_list->empty_leave_counts[i] / total_count);
  }
  return mean;
}

int leave_list_get_count_index(const LeaveList *leave_list, int klv_index) {
  return leave_list->leaves_ordered_by_klv_index[klv_index]->count_index;
}

const EncodedRack *leave_list_get_encoded_rack(const LeaveList *leave_list,
                                               int klv_index) {
  return &leave_list->leaves_ordered_by_klv_index[klv_index]->encoded_rack;
}