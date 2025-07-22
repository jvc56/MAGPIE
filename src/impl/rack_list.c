#include "rack_list.h"

#include <pthread.h>
#include <stdint.h>

#include "../ent/dictionary_word.h"
#include "../ent/encoded_rack.h"
#include "../ent/klv.h"
#include "../ent/kwg.h"
#include "../ent/letter_distribution.h"
#include "../ent/rack.h"
#include "../ent/xoshiro.h"

#include "../def/klv_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/rack_defs.h"

#include "kwg_maker.h"

#include "../util/io_util.h"
#include "../util/math_util.h"

typedef struct RackListItem {
  // Index of this item in the rack list items ordered by count.
  int count_index;
  int count;
  double mean;
  uint64_t total_combos;
  EncodedRack encoded_rack;
  pthread_mutex_t mutex;
} RackListItem;

struct RackList {
  int number_of_racks;
  // Racks with this count are no longer considered
  // rare and are excluded from forced draws.
  int target_rack_count;
  // Index of the last rack in the racks_partitioned_by_target_count
  // which has a count less than target_rack_count. All racks
  // with an index greater than this are no longer considered rare.
  // All racks with an index less than or equal to this are considered rare.
  int partition_index;
  pthread_mutex_t partition_index_mutex;
  uint64_t total_combos_sum;
  KLV *klv;
  RackListItem **racks_ordered_by_index;
  RackListItem **racks_partitioned_by_target_count;
};

typedef enum {
  RACK_GEN_MODE_CREATE_DWL,
  RACK_GEN_MODE_SET_RACK_LIST_ITEMS,
} rack_gen_mode_t;

// Smaller, mutable version of the LetterDistribution struct.
typedef struct RackListLetterDistribution {
  int size;
  int distribution[MACHINE_LETTER_MAX_VALUE];
} RackListLetterDistribution;

void rack_list_ld_init(RackListLetterDistribution *rl_ld,
                       const LetterDistribution *ld) {
  rl_ld->size = ld_get_size(ld);
  for (int i = 0; i < ld->size; i++) {
    rl_ld->distribution[i] = ld_get_dist(ld, i);
  }
}

void rack_list_ld_increment(RackListLetterDistribution *rl_ld, int index,
                            int amount) {
  rl_ld->distribution[index] += amount;
}

void rack_list_ld_decrement(RackListLetterDistribution *rl_ld, int index,
                            int amount) {
  rl_ld->distribution[index] -= amount;
}

int rack_list_ld_get_dist(const RackListLetterDistribution *rl_ld, int index) {
  return rl_ld->distribution[index];
}

int rack_list_ld_get_size(const RackListLetterDistribution *rl_ld) {
  return rl_ld->size;
}

int convert_klv_index_to_rack_list_index(int klv_index) {
  // cppcheck-suppress integerOverflowCond
  return klv_index - (RACK_SIZE);
}

int convert_word_index_to_rack_list_index(int word_index) {
  return word_index - (RACK_SIZE) + 1;
}

RackListItem *rack_list_item_create(int count_index) {
  RackListItem *item = malloc_or_die(sizeof(RackListItem));
  item->count = 0;
  item->mean = 0;
  item->count_index = count_index;
  item->total_combos = 0;
  pthread_mutex_init(&item->mutex, NULL);
  return item;
}

uint64_t get_total_combos_for_rack(const RackListLetterDistribution *rl_ld,
                                   const Rack *rack) {
  uint64_t total_combos = 1;
  const int ld_size = rack_list_ld_get_size(rl_ld);
  for (int ml = 0; ml < ld_size; ml++) {
    const int8_t num_ml = rack_get_letter(rack, ml);
    if (num_ml == 0) {
      continue;
    }
    total_combos *= choose(rack_list_ld_get_dist(rl_ld, ml), num_ml);
  }
  return total_combos;
}

int rack_list_generate_all_racks(rack_gen_mode_t mode,
                                 const RackListLetterDistribution *rl_ld,
                                 Rack *rack, MachineLetter ml,
                                 DictionaryWordList *dwl,
                                 MachineLetter *rack_array, RackList *rack_list,
                                 uint32_t node_index, uint32_t klv_index) {
  int number_of_set_racks = 0;
  const uint32_t ld_size = rack_list_ld_get_size(rl_ld);
  while (ml < ld_size &&
         rack_list_ld_get_dist(rl_ld, ml) - rack_get_letter(rack, ml) == 0) {
    ml++;
  }
  const int number_of_letters_in_rack = rack_get_total_letters(rack);
  if (ml == ld_size) {
    if (number_of_letters_in_rack == (RACK_SIZE)) {
      number_of_set_racks++;
      switch (mode) {
      case RACK_GEN_MODE_CREATE_DWL:
        dictionary_word_list_add_word(dwl, rack_array, (RACK_SIZE));
        break;
      case RACK_GEN_MODE_SET_RACK_LIST_ITEMS:
        if (klv_index == KLV_UNFOUND_INDEX) {
          log_fatal("word index not found in klv: %u", klv_index);
        }
        const uint32_t rack_list_index =
            convert_klv_index_to_rack_list_index((int)klv_index);
        rack_encode(
            rack,
            &rack_list->racks_ordered_by_index[rack_list_index]->encoded_rack);
        const uint64_t total_combos = get_total_combos_for_rack(rl_ld, rack);
        rack_list->racks_ordered_by_index[rack_list_index]->total_combos =
            total_combos;
        rack_list->total_combos_sum += total_combos;
        break;
      }
    }
  } else {
    number_of_set_racks +=
        rack_list_generate_all_racks(mode, rl_ld, rack, ml + 1, dwl, rack_array,
                                     rack_list, node_index, klv_index);
    int num_ml_to_add =
        rack_list_ld_get_dist(rl_ld, ml) - rack_get_letter(rack, ml);
    if (number_of_letters_in_rack + num_ml_to_add > (RACK_SIZE)) {
      num_ml_to_add = (RACK_SIZE)-number_of_letters_in_rack;
    }
    for (int i = 0; i < num_ml_to_add; i++) {
      rack_add_letter(rack, ml);
      switch (mode) {
      case RACK_GEN_MODE_CREATE_DWL:
        rack_array[rack_get_total_letters(rack) - 1] = ml;
        break;
      case RACK_GEN_MODE_SET_RACK_LIST_ITEMS:;
        uint32_t sibling_klv_index;
        node_index = increment_node_to_ml(rack_list->klv, node_index, klv_index,
                                          &sibling_klv_index, ml);
        klv_index = sibling_klv_index;
        uint32_t child_klv_index;
        node_index =
            follow_arc(rack_list->klv, node_index, klv_index, &child_klv_index);
        klv_index = child_klv_index;
        break;
      }
      number_of_set_racks += rack_list_generate_all_racks(
          mode, rl_ld, rack, ml + 1, dwl, rack_array, rack_list, node_index,
          klv_index);
    }
    rack_take_letters(rack, ml, num_ml_to_add);
  }
  return number_of_set_racks;
}

void rack_list_item_reset(RackListItem *item) {
  item->count = 0;
  item->mean = 0;
}

RackList *rack_list_create(const LetterDistribution *ld,
                           int target_rack_count) {
  RackList *rack_list = malloc_or_die(sizeof(RackList));
  rack_list->total_combos_sum = 0;

  Rack rack;
  rack_set_dist_size(&rack, ld_get_size(ld));
  rack_reset(&rack);

  DictionaryWordList *dwl = dictionary_word_list_create();

  MachineLetter rack_array[RACK_SIZE];
  RackListLetterDistribution rl_ld;
  rack_list_ld_init(&rl_ld, ld);

  rack_list->number_of_racks = rack_list_generate_all_racks(
      RACK_GEN_MODE_CREATE_DWL, &rl_ld, &rack, 0, dwl, rack_array, NULL, 0, 0);

  KWG *kwg =
      make_kwg_from_words(dwl, KWG_MAKER_OUTPUT_DAWG, KWG_MAKER_MERGE_EXACT);

  dictionary_word_list_destroy(dwl);

  rack_list->klv = klv_create_zeroed_from_kwg(kwg, rack_list->number_of_racks,
                                              "internal_rack_list_klv");

  const size_t racks_malloc_size =
      sizeof(RackListItem *) * rack_list->number_of_racks;
  rack_list->racks_ordered_by_index = malloc_or_die(racks_malloc_size);
  for (int i = 0; i < rack_list->number_of_racks; i++) {
    rack_list->racks_ordered_by_index[i] = rack_list_item_create(i);
  }

  rack_list_generate_all_racks(RACK_GEN_MODE_SET_RACK_LIST_ITEMS, &rl_ld, &rack,
                               0, NULL, rack_array, rack_list,
                               kwg_get_dawg_root_node_index(kwg), 0);

  rack_list->racks_partitioned_by_target_count =
      malloc_or_die(racks_malloc_size);

  memcpy(rack_list->racks_partitioned_by_target_count,
         rack_list->racks_ordered_by_index, racks_malloc_size);

  rack_list->partition_index = rack_list->number_of_racks - 1;
  pthread_mutex_init(&rack_list->partition_index_mutex, NULL);
  rack_list->target_rack_count = target_rack_count;

  return rack_list;
}

void rack_list_destroy(RackList *rack_list) {
  if (!rack_list) {
    return;
  }
  klv_destroy(rack_list->klv);
  for (int i = 0; i < rack_list->number_of_racks; i++) {
    free(rack_list->racks_ordered_by_index[i]);
  }
  free(rack_list->racks_ordered_by_index);
  free(rack_list->racks_partitioned_by_target_count);
  free(rack_list);
}

void rack_list_reset(RackList *rack_list, int target_rack_count) {
  klv_set_all_leave_values_to_zero(rack_list->klv);
  for (int i = 0; i < rack_list->number_of_racks; i++) {
    rack_list_item_reset(rack_list->racks_partitioned_by_target_count[i]);
  }
  rack_list->partition_index = rack_list->number_of_racks - 1;
  rack_list->target_rack_count = target_rack_count;
}

void rack_list_item_increment_count(RackListItem *item, double equity) {
  item->count++;
  item->mean += (1.0 / item->count) * (equity - item->mean);
}

void rack_list_swap_items(RackList *rack_list, int i, int j) {
  // Perform the swap
  RackListItem *temp = rack_list->racks_partitioned_by_target_count[i];
  rack_list->racks_partitioned_by_target_count[i] =
      rack_list->racks_partitioned_by_target_count[j];
  rack_list->racks_partitioned_by_target_count[j] = temp;

  // Reassign count indexes
  rack_list->racks_partitioned_by_target_count[i]->count_index = i;
  rack_list->racks_partitioned_by_target_count[j]->count_index = j;
}

void rack_list_add_rack_with_rack_list_index(RackList *rack_list,
                                             int rack_list_index,
                                             double equity) {
  RackListItem *item = rack_list->racks_ordered_by_index[rack_list_index];
  bool item_reached_target_count = false;
  pthread_mutex_lock(&item->mutex);
  rack_list_item_increment_count(item, equity);
  item_reached_target_count = item->count == rack_list->target_rack_count;
  pthread_mutex_unlock(&item->mutex);

  if (item_reached_target_count) {
    pthread_mutex_lock(&rack_list->partition_index_mutex);
    const int curr_partition_index = rack_list->partition_index;
    if (curr_partition_index >= item->count_index) {
      if (curr_partition_index != item->count_index) {
        RackListItem *swap_item =
            rack_list->racks_partitioned_by_target_count[curr_partition_index];
        pthread_mutex_lock(&item->mutex);
        pthread_mutex_lock(&swap_item->mutex);
        rack_list_swap_items(rack_list, item->count_index,
                             swap_item->count_index);
        pthread_mutex_unlock(&swap_item->mutex);
        pthread_mutex_unlock(&item->mutex);
      }
      rack_list->partition_index--;
    }
    pthread_mutex_unlock(&rack_list->partition_index_mutex);
  }
}

// Adds a single rack to the list.
void rack_list_add_rack(RackList *rack_list, const Rack *rack, double equity) {
  rack_list_add_rack_with_rack_list_index(
      rack_list,
      convert_word_index_to_rack_list_index(
          klv_get_word_index(rack_list->klv, rack)),
      equity);
}

int rack_list_get_racks_below_target_count(const RackList *rack_list) {
  return rack_list->partition_index + 1;
}

// Returns false if there are no rare racks remaining in the list.
bool rack_list_get_rare_rack(RackList *rack_list, XoshiroPRNG *prng,
                             Rack *rack) {
  // Since we don't lock the partition index, it's possible that the
  // selected random rack will have already reached the minimum target
  // leave count and no longer be rare, but that's okay since we
  // can still use the rack and most of the time the rack will still
  // be rare. Not having to lock every time we need a rare rack is worth
  // occasionally incrementing a rack that is no longer rare due to race
  // conditions.
  if (rack_list_get_racks_below_target_count(rack_list) == 0) {
    return false;
  }
  const int random_rack_index =
      prng_get_random_number(prng, rack_list->partition_index + 1);
  rack_decode(&rack_list->racks_partitioned_by_target_count[random_rack_index]
                   ->encoded_rack,
              rack);
  return true;
}

typedef struct RackListLeave {
  double equity_sum;
  uint64_t count_sum;
} RackListLeave;

void generate_leaves(RackListLeave *leave_list, const KLV *klv,
                     double rack_equity, Rack *full_rack,
                     RackListLetterDistribution *rl_ld, Rack *leave,
                     uint32_t node_index, uint32_t word_index,
                     MachineLetter ml) {
  const uint16_t ld_size = rack_get_dist_size(full_rack);
  while (ml < ld_size && rack_get_letter(full_rack, ml) == 0) {
    ml++;
  }
  if (ml == ld_size) {
    const int number_of_letters_in_leave = rack_get_total_letters(leave);
    if (number_of_letters_in_leave > 0 &&
        number_of_letters_in_leave < (RACK_SIZE)) {
      // Count the number of ways we can draw the remaining letters for this
      // rack after the leave is subtracted from the letter distribution.
      const uint64_t count = get_total_combos_for_rack(rl_ld, full_rack);
      leave_list[word_index - 1].count_sum += count;
      leave_list[word_index - 1].equity_sum += rack_equity * count;
    }
  } else {
    generate_leaves(leave_list, klv, rack_equity, full_rack, rl_ld, leave,
                    node_index, word_index, ml + 1);
    const int8_t num_this = rack_get_letter(full_rack, ml);
    for (int8_t i = 0; i < num_this; i++) {
      rack_add_letter(leave, ml);
      rack_take_letter(full_rack, ml);
      rack_list_ld_decrement(rl_ld, ml, 1);
      uint32_t sibling_word_index;
      node_index = increment_node_to_ml(klv, node_index, word_index,
                                        &sibling_word_index, ml);
      word_index = sibling_word_index;
      uint32_t child_word_index;
      node_index = follow_arc(klv, node_index, word_index, &child_word_index);
      word_index = child_word_index;
      generate_leaves(leave_list, klv, rack_equity, full_rack, rl_ld, leave,
                      node_index, word_index, ml + 1);
    }
    rack_take_letters(leave, ml, num_this);
    rack_add_letters(full_rack, ml, num_this);
    rack_list_ld_increment(rl_ld, ml, num_this);
  }
}

void rack_list_write_to_klv(RackList *rack_list, const LetterDistribution *ld,
                            KLV *klv) {
  double weighted_sum = 0.0;
  const int ld_size = ld_get_size(ld);
  Rack rack;
  rack_set_dist_size(&rack, ld_size);
  for (int i = 0; i < rack_list->number_of_racks; i++) {
    const RackListItem *rli = rack_list->racks_ordered_by_index[i];
    weighted_sum += rli->mean * (double)rli->total_combos;
  }
  double average_equity = weighted_sum / (double)rack_list->total_combos_sum;

  const int klv_number_of_leaves = klv_get_number_of_leaves(klv);
  RackListLeave *leave_list =
      malloc_or_die(sizeof(RackListLeave) * klv_number_of_leaves);
  for (int i = 0; i < klv_number_of_leaves; i++) {
    leave_list[i].count_sum = 0;
    leave_list[i].equity_sum = 0;
  }

  Rack leave;
  rack_set_dist_size(&leave, ld_size);
  RackListLetterDistribution rl_ld;
  rack_list_ld_init(&rl_ld, ld);
  for (int i = 0; i < rack_list->number_of_racks; i++) {
    const RackListItem *rli = rack_list->racks_ordered_by_index[i];
    rack_decode(&rli->encoded_rack, &rack);
    rack_reset(&leave);
    generate_leaves(leave_list, klv, rli->mean, &rack, &rl_ld, &leave,
                    kwg_get_dawg_root_node_index(klv->kwg), 0, 0);
  }
  for (int i = 0; i < klv_number_of_leaves; i++) {
    if (leave_list[i].count_sum > 0) {
      klv->leave_values[i] = double_to_equity(
          (leave_list[i].equity_sum / leave_list[i].count_sum) -
          average_equity);
    } else {
      klv->leave_values[i] = 0;
    }
  }
  free(leave_list);
}

int rack_list_get_target_rack_count(const RackList *rack_list) {
  return rack_list->target_rack_count;
}

int rack_list_get_number_of_racks(const RackList *rack_list) {
  return rack_list->number_of_racks;
}

uint64_t rack_list_get_count(const RackList *rack_list, int klv_index) {
  return rack_list
      ->racks_ordered_by_index[convert_klv_index_to_rack_list_index(klv_index)]
      ->count;
}

double rack_list_get_mean(const RackList *rack_list, int klv_index) {
  return rack_list
      ->racks_ordered_by_index[convert_klv_index_to_rack_list_index(klv_index)]
      ->mean;
}

const EncodedRack *rack_list_get_encoded_rack(const RackList *rack_list,
                                              int klv_index) {
  return &rack_list
              ->racks_ordered_by_index[convert_klv_index_to_rack_list_index(
                  klv_index)]
              ->encoded_rack;
}

const KLV *rack_list_get_klv(const RackList *rack_list) {
  return rack_list->klv;
}