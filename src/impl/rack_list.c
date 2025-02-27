#include "rack_list.h"

#include <pthread.h>

#include "../ent/bag.h"
#include "../ent/encoded_rack.h"
#include "../ent/game.h"
#include "../ent/klv.h"
#include "../ent/rack.h"
#include "../ent/thread_control.h"
#include "../ent/xoshiro.h"

#include "kwg_maker.h"

#include "../str/rack_string.h"

#include "../util/util.h"

typedef struct RackListItem {
  // Index of this item in the rack list items ordered by count.
  int count_index;
  int count;
  double mean;
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
  KLV *klv;
  RackListItem **racks_ordered_by_klv_index;
  RackListItem **racks_partitioned_by_target_count;
};

typedef enum {
  RACK_GEN_MODE_CREATE_DWL,
  RACK_GEN_MODE_SET_RACK_LIST_ITEMS,
} rack_gen_mode_t;

RackListItem *rack_list_item_create(int count_index) {
  RackListItem *item = malloc_or_die(sizeof(RackListItem));
  item->count = 0;
  item->mean = 0;
  item->count_index = count_index;
  pthread_mutex_init(&item->mutex, NULL);
  return item;
}

void print_er(const Rack *rack) {
  for (int i = 0; i < rack_get_letter(rack, BLANK_MACHINE_LETTER); i++) {
    printf("?");
  }
  const uint16_t ld_size = rack_get_dist_size(rack);
  for (int i = 1; i < ld_size; i++) {
    const int num_letter = rack_get_letter(rack, i);
    for (int j = 0; j < num_letter; j++) {
      printf("%c", i + 'A' - 1);
    }
  }
}

int rack_list_generate_all_racks(rack_gen_mode_t mode,
                                 const LetterDistribution *ld, Rack *rack,
                                 uint8_t ml, DictionaryWordList *dwl,
                                 uint8_t *rack_array, RackList *rack_list,
                                 uint32_t node_index, uint32_t word_index) {
  int number_of_set_racks = 0;
  const uint32_t ld_size = ld_get_size(ld);
  while (ml < ld_size && ld_get_dist(ld, ml) - rack_get_letter(rack, ml) == 0) {
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
        if (word_index == KLV_UNFOUND_INDEX) {
          log_fatal("word index not found in klv: %u", word_index);
        }
        // FIXME: find out why - RACK_SIZE is needed
        const uint32_t rack_list_index = word_index - (RACK_SIZE);
        rack_encode(rack,
                    &rack_list->racks_ordered_by_klv_index[rack_list_index]
                         ->encoded_rack);
        break;
      }
    }
  } else {
    number_of_set_racks +=
        rack_list_generate_all_racks(mode, ld, rack, ml + 1, dwl, rack_array,
                                     rack_list, node_index, word_index);
    int num_ml_to_add = ld_get_dist(ld, ml) - rack_get_letter(rack, ml);
    if (number_of_letters_in_rack + num_ml_to_add > (RACK_SIZE)) {
      num_ml_to_add = (RACK_SIZE)-number_of_letters_in_rack;
    }
    for (int i = 0; i < num_ml_to_add; i++) {
      rack_add_letter(rack, ml);
      switch (mode) {
      case RACK_GEN_MODE_CREATE_DWL:
        rack_array[rack_get_total_letters(rack) - 1] = ml;
        break;
      case RACK_GEN_MODE_SET_RACK_LIST_ITEMS:
          // Empty statement to allow declaration
          ;
        uint32_t sibling_word_index;
        node_index = increment_node_to_ml(rack_list->klv, node_index,
                                          word_index, &sibling_word_index, ml);
        word_index = sibling_word_index;
        uint32_t child_word_index;
        node_index = follow_arc(rack_list->klv, node_index, word_index,
                                &child_word_index);
        word_index = child_word_index;
        break;
      }
      number_of_set_racks +=
          rack_list_generate_all_racks(mode, ld, rack, ml + 1, dwl, rack_array,
                                       rack_list, node_index, word_index);
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

  Rack rack;
  rack_set_dist_size(&rack, ld_get_size(ld));
  rack_reset(&rack);

  DictionaryWordList *dwl = dictionary_word_list_create();

  uint8_t rack_array[RACK_SIZE];

  rack_list->number_of_racks = rack_list_generate_all_racks(
      RACK_GEN_MODE_CREATE_DWL, ld, &rack, 0, dwl, rack_array, NULL, 0, 0);

  KWG *kwg =
      make_kwg_from_words(dwl, KWG_MAKER_OUTPUT_DAWG, KWG_MAKER_MERGE_EXACT);

  dictionary_word_list_destroy(dwl);

  const size_t racks_malloc_size =
      sizeof(RackListItem *) * rack_list->number_of_racks;
  rack_list->racks_ordered_by_klv_index = malloc_or_die(racks_malloc_size);
  for (int i = 0; i < rack_list->number_of_racks; i++) {
    rack_list->racks_ordered_by_klv_index[i] = rack_list_item_create(i);
  }

  rack_list->klv = klv_create_zeroed_from_kwg(kwg, rack_list->number_of_racks,
                                              "internal_rack_list_klv");

  rack_list_generate_all_racks(RACK_GEN_MODE_SET_RACK_LIST_ITEMS, ld, &rack, 0,
                               NULL, NULL, rack_list,
                               kwg_get_dawg_root_node_index(kwg), 0);

  rack_list->racks_partitioned_by_target_count =
      malloc_or_die(racks_malloc_size);

  memory_copy(rack_list->racks_partitioned_by_target_count,
              rack_list->racks_ordered_by_klv_index, racks_malloc_size);

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
    free(rack_list->racks_ordered_by_klv_index[i]);
  }
  free(rack_list->racks_ordered_by_klv_index);
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

void rack_list_add_rack_with_klv_index(RackList *rack_list, int klv_index,
                                       double equity) {
  RackListItem *item = rack_list->racks_ordered_by_klv_index[klv_index];
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
void rack_list_add_single_rack(RackList *rack_list, const Rack *rack,
                               double equity) {
  rack_list_add_rack_with_klv_index(
      rack_list, klv_get_word_index(rack_list->klv, rack), equity);
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

int rack_list_get_target_rack_count(const RackList *rack_list) {
  return rack_list->target_rack_count;
}

int rack_list_get_number_of_racks(const RackList *rack_list) {
  return rack_list->number_of_racks;
}

uint64_t rack_list_get_count(const RackList *rack_list, int klv_index) {
  return rack_list->racks_ordered_by_klv_index[klv_index]->count;
}

double rack_list_get_mean(const RackList *rack_list, int klv_index) {
  return rack_list->racks_ordered_by_klv_index[klv_index]->mean;
}

int rack_list_get_count_index(const RackList *rack_list, int klv_index) {
  return rack_list->racks_ordered_by_klv_index[klv_index]->count_index;
}

const EncodedRack *rack_list_get_encoded_rack(const RackList *rack_list,
                                              int klv_index) {
  return &rack_list->racks_ordered_by_klv_index[klv_index]->encoded_rack;
}