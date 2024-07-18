#include "../ent/klv.h"
#include "../ent/leave_list.h"
#include "../ent/letter_distribution.h"
#include "../ent/rack.h"

#include "../str/rack_string.h"

void add_leave_to_klv_builder(const KLV *klv, const LetterDistribution *ld,
                              StringBuilder *klv_builder, const Rack *leave,
                              int index) {
  string_builder_add_rack(klv_builder, leave, ld);
  string_builder_add_formatted_string(klv_builder, ",%f",
                                      klv_get_indexed_leave_value(klv, index));
}

// FIXME: use this in other files maybe?
void get_next_node_and_word_index(const KLV *klv, uint32_t *node_index,
                                  uint32_t *word_index, uint8_t ml) {
  uint32_t sibling_word_index;
  *node_index = increment_node_to_ml(klv, *node_index, *word_index,
                                     &sibling_word_index, ml);
  *word_index = sibling_word_index;
  uint32_t child_word_index;
  *node_index = follow_arc(klv, *node_index, *word_index, &child_word_index);
  *word_index = child_word_index;
}

void klv_write_for_length_recur(const KLV *klv, const LetterDistribution *ld,
                                int length, StringBuilder *klv_builder,
                                Rack *bag_as_rack, Rack *leave,
                                uint32_t node_index, uint32_t word_index,
                                uint8_t ml) {
  const int dist_size = rack_get_dist_size(leave);
  if (ml == dist_size) {
    return;
  }

  if (rack_get_total_letters(leave) == length) {
    add_leave_to_klv_builder(klv, ld, klv_builder, leave, word_index - 1);
    return;
  }

  for (int i = ml; i < dist_size; i++) {
    if (rack_get_letter(bag_as_rack, i) > 0) {
      rack_take_letter(bag_as_rack, i);
      rack_add_letter(leave, i);
      get_next_node_and_word_index(klv, &node_index, &word_index, i);
      klv_write_for_length_recur(klv, ld, length, klv_builder, bag_as_rack,
                                 leave, node_index, word_index, i);
      rack_add_letter(bag_as_rack, i);
      rack_take_letter(leave, i);
    }
  }
}

void klv_write_for_length(const KLV *klv, const LetterDistribution *ld,
                          StringBuilder *klv_builder, Rack *bag_as_rack,
                          Rack *leave, int length) {
  klv_write_for_length_recur(klv, ld, length, klv_builder, bag_as_rack, leave,
                             kwg_get_dawg_root_node_index(klv->kwg), 0, 0);
}

void klv_write(const KLV *klv, const LetterDistribution *ld,
               const char *filepath) {
  const int dist_size = ld_get_size(ld);
  Rack *leave = rack_create(dist_size);
  Rack *bag_as_rack = get_new_bag_as_rack(ld);
  StringBuilder *klv_builder = string_builder_create();

  for (int i = 1; i < (RACK_SIZE); i++) {
    klv_write_for_length(klv, ld, klv_builder, leave, filepath, i);
  }

  write_string_to_file(filepath, "w", string_builder_peek(klv_builder));
  string_builder_destroy(klv_builder);
  rack_destroy(leave);
  rack_destroy(bag_as_rack);
}
