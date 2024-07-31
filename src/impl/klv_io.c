#include "../ent/dictionary_word.h"
#include "../ent/klv.h"
#include "../ent/leave_list.h"
#include "../ent/letter_distribution.h"
#include "../ent/rack.h"

#include "../impl/kwg_maker.h"

#include "../str/rack_string.h"

typedef void (*leave_iter_func_t)(void *);

typedef struct LeaveIter {
  leave_iter_func_t leave_iter_func;
  void *data;
} LeaveIter;

typedef struct KLVWriteData {
  const KLV *klv;
  const LetterDistribution *ld;
  const Rack *leave;
  StringBuilder *string_builder;
} KLVWriteData;

typedef struct KLVCreateData {
  const Rack *leave;
  DictionaryWordList *dwl;
} KLVCreateData;

void klv_write_row(void *data) {
  KLVWriteData *lasb = (KLVWriteData *)data;
  string_builder_add_rack(lasb->string_builder, lasb->leave, lasb->ld);
  string_builder_add_formatted_string(
      lasb->string_builder, ",%f",
      klv_get_indexed_leave_value(lasb->klv, index));
}

void klv_add_leave_to_word_list(void *data) {
  KLVCreateData *klv_data = (KLVCreateData *)data;
  uint8_t word[(RACK_SIZE)-1];
  int letter_index = 0;
  const int dist_size = rack_get_dist_size(klv_data->leave);
  for (int i = 0; i < dist_size; i++) {
    for (int j = 0; j < rack_get_letter(klv_data->leave, i); j++) {
      word[letter_index++] = i;
    }
  }
  dictionary_word_list_add_word(klv_data->dwl, word, letter_index);
}

// FIXME: use this in other files maybe?
void get_next_node_and_word_index(const KLV *klv, uint32_t *node_index,
                                  uint32_t *word_index, uint8_t ml) {
  if (!klv) {
    return;
  }
  uint32_t sibling_word_index;
  *node_index = increment_node_to_ml(klv, *node_index, *word_index,
                                     &sibling_word_index, ml);
  *word_index = sibling_word_index;
  uint32_t child_word_index;
  *node_index = follow_arc(klv, *node_index, *word_index, &child_word_index);
  *word_index = child_word_index;
}

void klv_iter_for_length_recur(LeaveIter *leave_iter, KLV *klv, int length,
                               Rack *bag_as_rack, Rack *leave,
                               uint32_t node_index, uint32_t word_index,
                               uint8_t ml) {
  const int dist_size = rack_get_dist_size(leave);
  if (ml == dist_size) {
    return;
  }

  if (rack_get_total_letters(leave) == length) {
    leave_iter->leave_iter_func(leave_iter->data);
    return;
  }

  for (int i = ml; i < dist_size; i++) {
    if (rack_get_letter(bag_as_rack, i) > 0) {
      rack_take_letter(bag_as_rack, i);
      rack_add_letter(leave, i);
      get_next_node_and_word_index(klv, &node_index, &word_index, i);
      klv_iter_for_length_recur(leave_iter, klv, length, bag_as_rack, leave,
                                node_index, word_index, i);
      rack_add_letter(bag_as_rack, i);
      rack_take_letter(leave, i);
    }
  }
}

void klv_iter_for_length(LeaveIter *leave_iter, KLV *klv, Rack *bag_as_rack,
                         Rack *leave, int length) {
  klv_iter_for_length_recur(leave_iter, klv, length, bag_as_rack, leave,
                            kwg_get_dawg_root_node_index(klv->kwg), 0, 0);
}

// Writes a CSV file of leave,value for the leaves in the KLV.
void klv_write(const KLV *klv, const LetterDistribution *ld,
               const char *filepath) {
  const int dist_size = ld_get_size(ld);
  Rack *leave = rack_create(dist_size);
  Rack *bag_as_rack = get_new_bag_as_rack(ld);
  StringBuilder *klv_builder = string_builder_create();

  KLVWriteData klv_write_data;
  klv_write_data.klv = klv;
  klv_write_data.ld = ld;
  klv_write_data.leave = leave;
  klv_write_data.string_builder = klv_builder;

  LeaveIter leave_iter;
  leave_iter.data = (void *)&klv_write_data;
  leave_iter.leave_iter_func = klv_write_row;

  for (int i = 1; i < (RACK_SIZE); i++) {
    klv_iter_for_length(&leave_iter, klv, bag_as_rack, leave, i);
  }

  write_string_to_file(filepath, "w", string_builder_peek(klv_builder));
  string_builder_destroy(klv_builder);
  rack_destroy(leave);
  rack_destroy(bag_as_rack);
}

KLV *klv_create_empty(const LetterDistribution *ld) {
  const int dist_size = ld_get_size(ld);
  Rack *leave = rack_create(dist_size);
  Rack *bag_as_rack = get_new_bag_as_rack(ld);

  KLVCreateData klv_create_data;
  klv_create_data.dwl = dictionary_word_list_create();
  klv_create_data.leave = leave;

  LeaveIter leave_iter;
  leave_iter.data = (void *)&klv_create_data;
  leave_iter.leave_iter_func = klv_add_leave_to_word_list;

  for (int i = 1; i < (RACK_SIZE); i++) {
    klv_iter_for_length(&leave_iter, NULL, bag_as_rack, leave, i);
  }

  KWG *kwg = make_kwg_from_words(klv_create_data.dwl, KWG_MAKER_OUTPUT_DAWG,
                                 KWG_MAKER_MERGE_EXACT);
}

// Reads a CSV file of leave,value and returns a KLV.
KLV *klv_read(const LetterDistribution *ld, const char *filepath) {}