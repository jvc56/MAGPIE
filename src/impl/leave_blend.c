#include "leave_blend.h"

#include "../def/klv_defs.h"
#include "../def/kwg_defs.h"
#include "../ent/dictionary_word.h"
#include "../ent/equity.h"
#include "../ent/klv.h"
#include "../ent/kwg.h"
#include "../ent/rack.h"
#include "../util/io_util.h"
#include "kwg_maker.h"
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The leave's value in klv, and whether klv holds it.
static bool leave_blend_lookup(const KLV *klv, const Rack *leave,
                               Equity *value) {
  const uint32_t index = klv_get_word_index(klv, leave);
  if (index == KLV_UNFOUND_INDEX) {
    return false;
  }
  *value = klv_get_indexed_leave_value(klv, index);
  return true;
}

void leave_blend(const char *data_paths, const char *first_name,
                 const char *second_name, double weight,
                 const char *output_name, ErrorStack *error_stack) {
  if (!(weight >= 0.0 && weight <= 1.0)) {
    error_stack_push(
        error_stack, ERROR_STATUS_LEAVE_BLEND_INVALID_WEIGHT,
        get_formatted_string("the blend weight must be from 0 to 1, not %f",
                             weight));
    return;
  }
  KLV *first = klv_create(data_paths, first_name, error_stack);
  if (!first) {
    return;
  }
  KLV *second = klv_create(data_paths, second_name, error_stack);
  if (!second) {
    klv_destroy(first);
    return;
  }

  DictionaryWordList *all_leaves = dictionary_word_list_create();
  kwg_write_words(klv_get_kwg(first),
                  kwg_get_dawg_root_node_index(klv_get_kwg(first)), all_leaves,
                  NULL);
  kwg_write_words(klv_get_kwg(second),
                  kwg_get_dawg_root_node_index(klv_get_kwg(second)), all_leaves,
                  NULL);
  dictionary_word_list_sort(all_leaves);
  DictionaryWordList *leaves = dictionary_word_list_create();
  dictionary_word_list_unique(all_leaves, leaves);
  dictionary_word_list_destroy(all_leaves);

  const int number_of_leaves = dictionary_word_list_get_count(leaves);
  KWG *kwg =
      make_kwg_from_words(leaves, KWG_MAKER_OUTPUT_DAWG, KWG_MAKER_MERGE_EXACT);
  KLV *blend = klv_create_zeroed_from_kwg(kwg, number_of_leaves, output_name);

  int max_letter = 0;
  for (int leave_idx = 0; leave_idx < number_of_leaves; leave_idx++) {
    const DictionaryWord *word =
        dictionary_word_list_get_word(leaves, leave_idx);
    for (int letter_idx = 0; letter_idx < dictionary_word_get_length(word);
         letter_idx++) {
      if (dictionary_word_get_word(word)[letter_idx] > max_letter) {
        max_letter = dictionary_word_get_word(word)[letter_idx];
      }
    }
  }
  Rack leave;
  rack_set_dist_size_and_reset(&leave, max_letter + 1);
  for (int leave_idx = 0; leave_idx < number_of_leaves; leave_idx++) {
    const DictionaryWord *word =
        dictionary_word_list_get_word(leaves, leave_idx);
    rack_reset(&leave);
    for (int letter_idx = 0; letter_idx < dictionary_word_get_length(word);
         letter_idx++) {
      rack_add_letter(&leave, dictionary_word_get_word(word)[letter_idx]);
    }
    Equity first_value = 0;
    Equity second_value = 0;
    const bool in_first = leave_blend_lookup(first, &leave, &first_value);
    const bool in_second = leave_blend_lookup(second, &leave, &second_value);
    Equity value;
    if (in_first && in_second) {
      value = (Equity)llround((1.0 - weight) * (double)first_value +
                              weight * (double)second_value);
    } else if (in_first) {
      value = first_value;
    } else {
      value = second_value;
    }
    klv_set_indexed_leave_value(blend, klv_get_word_index(blend, &leave),
                                value);
  }
  dictionary_word_list_destroy(leaves);
  klv_write(blend, data_paths, output_name, error_stack);
  klv_destroy(blend);
  klv_destroy(second);
  klv_destroy(first);
}
