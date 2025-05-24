#ifndef KLV_H
#define KLV_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../def/klv_defs.h"

#include "../ent/kwg.h"

#include "../compat/endian_conv.h"

#include "../util/fileproxy.h"
#include "../util/io_util.h"
#include "../util/string_util.h"

#include "data_filepaths.h"
#include "kwg.h"
#include "rack.h"

// The KLV data structure was originally
// developed in wolges. For more details
// on how the KLV data structure works, see
// https://github.com/andy-k/wolges/blob/main/details.txt
typedef struct KLV {
  int number_of_leaves;
  KWG *kwg;
  char *name;
  uint32_t *word_counts;
  Equity *leave_values;
} KLV;

static inline const char *klv_get_name(const KLV *klv) { return klv->name; }

static inline const KWG *klv_get_kwg(const KLV *klv) { return klv->kwg; }

static inline int klv_get_number_of_leaves(const KLV *klv) {
  return klv->number_of_leaves;
}

static inline void klv_set_all_leave_values_to_zero(KLV *klv) {
  memset(klv->leave_values, int_to_equity(0),
         klv->number_of_leaves * sizeof(Equity));
}

static inline Equity klv_get_indexed_leave_value(const KLV *klv,
                                                 uint32_t index) {
  if (index == KLV_UNFOUND_INDEX) {
    return 0.0;
  }
  return klv->leave_values[index];
}

// Assumes the index is valid
static inline Equity klv_set_indexed_leave_value(const KLV *klv, uint32_t index,
                                                 Equity value) {
  return klv->leave_values[index] = value;
}

static inline uint32_t klv_get_root_node_index(const KLV *klv) {
  const uint32_t dawg_pointer_node = kwg_node(klv->kwg, 0);
  return kwg_node_arc_index(dawg_pointer_node);
}

static inline uint32_t increment_node_to_ml(const KLV *klv, uint32_t node_index,
                                            uint32_t word_index,
                                            uint32_t *next_word_index,
                                            uint8_t ml) {
  if (node_index == 0) {
    *next_word_index = KLV_UNFOUND_INDEX;
    return 0;
  }
  uint32_t w_idx = word_index;
  for (;;) {
    const uint32_t node = kwg_node(klv->kwg, node_index);
    if (kwg_node_tile(node) == ml) {
      *next_word_index = w_idx;
      return node_index;
    }
    if (kwg_node_is_end(node)) {
      *next_word_index = KLV_UNFOUND_INDEX;
      return 0;
    }
    w_idx += klv->word_counts[node_index] - klv->word_counts[node_index + 1];
    node_index++;
  }
}

static inline uint32_t follow_arc(const KLV *klv, uint32_t node_index,
                                  uint32_t word_index,
                                  uint32_t *next_word_index) {
  if (node_index == 0) {
    *next_word_index = KLV_UNFOUND_INDEX;
    return 0;
  }
  *next_word_index = word_index + 1;
  const uint32_t node = kwg_node(klv->kwg, node_index);
  return kwg_node_arc_index(node);
}

static inline float reverse_float(const float in_float) {
  float ret_val;
  const char *float_to_convert = (char *)&in_float;
  char *return_float = (char *)&ret_val;

  // swap the bytes into a temporary buffer
  return_float[0] = float_to_convert[3];
  return_float[1] = float_to_convert[2];
  return_float[2] = float_to_convert[1];
  return_float[3] = float_to_convert[0];

  return ret_val;
}

static inline int klv_count_words_at(const KLV *klv, uint32_t node_index,
                                     uint32_t kwg_size) {
  if (node_index >= kwg_size) {
    return 0;
  }
  if (klv->word_counts[node_index] == KLV_UNFOUND_INDEX) {
    log_fatal("unexpected word count %u at %u", klv->word_counts[node_index],
              node_index);
  }
  if (klv->word_counts[node_index] == 0) {
    klv->word_counts[node_index] = KLV_UNFOUND_INDEX;

    const uint32_t node = kwg_node(klv->kwg, node_index);
    int this_node_word_count = 0;
    if (kwg_node_accepts(node)) {
      this_node_word_count = 1;
    }
    int child_word_count = 0;
    const uint32_t next_node_index = kwg_node_arc_index(node);
    if (next_node_index != 0) {
      child_word_count = klv_count_words_at(klv, next_node_index, kwg_size);
    }
    int sibling_word_count = 0;
    const bool is_not_end = !kwg_node_is_end(node);
    if (is_not_end) {
      sibling_word_count = klv_count_words_at(klv, node_index + 1, kwg_size);
    }
    klv->word_counts[node_index] =
        this_node_word_count + child_word_count + sibling_word_count;
  }
  return klv->word_counts[node_index];
}

static inline void klv_count_words(const KLV *klv, size_t kwg_size) {
  for (int p = kwg_size - 1; p >= 0; p--) {
    klv_count_words_at(klv, p, (int)kwg_size);
  }
}

static inline void klv_load(const char *klv_name, const char *klv_filename,
                            KLV *klv, ErrorStack *error_stack) {
  FILE *stream = stream_from_filename(klv_filename, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  klv->name = string_duplicate(klv_name);

  uint32_t kwg_size;
  size_t result;

  result = fread(&kwg_size, sizeof(kwg_size), 1, stream);
  if (result != 1) {
    log_fatal("kwg size fread failure: %zd != %d", result, 1);
  }
  kwg_size = le32toh(kwg_size);

  klv->kwg = kwg_create_empty();

  kwg_read_nodes_from_stream(klv->kwg, kwg_size, stream);

  uint32_t number_of_leaves;
  result = fread(&number_of_leaves, sizeof(number_of_leaves), 1, stream);
  if (result != 1) {
    log_fatal("number of leaves fread failure: %zd != %d", result, 1);
  }
  number_of_leaves = le32toh(number_of_leaves);
  klv->number_of_leaves = number_of_leaves;

  klv->leave_values =
      (Equity *)malloc_or_die(number_of_leaves * sizeof(Equity));
  float *temp_floats = (float *)malloc_or_die(number_of_leaves * sizeof(float));
  result = fread(temp_floats, sizeof(float), number_of_leaves, stream);
  if (result != number_of_leaves) {
    log_fatal("edges fread failure: %zd != %d", result, number_of_leaves);
  }

  fclose_or_die(stream);

  for (uint32_t i = 0; i < number_of_leaves; i++) {
    klv->leave_values[i] =
        double_to_equity((double)convert_float_to_le(temp_floats[i]));
  }
  free(temp_floats);

  klv->word_counts = malloc_or_die(kwg_size * sizeof(uint32_t));
  for (size_t i = 0; i < kwg_size; i++) {
    klv->word_counts[i] = 0;
  }

  klv_count_words(klv, kwg_size);
}

static inline void klv_destroy(KLV *klv) {
  if (!klv) {
    return;
  }
  kwg_destroy(klv->kwg);
  free(klv->leave_values);
  free(klv->word_counts);
  free(klv->name);
  free(klv);
}

static inline KLV *klv_create(const char *data_paths, const char *klv_name,
                              ErrorStack *error_stack) {

  char *klv_filename = data_filepaths_get_readable_filename(
      data_paths, klv_name, DATA_FILEPATH_TYPE_KLV, error_stack);
  KLV *klv = NULL;
  if (error_stack_is_empty(error_stack)) {
    klv = calloc_or_die(1, sizeof(KLV));
    klv_load(klv_name, klv_filename, klv, error_stack);
  }
  free(klv_filename);
  if (!error_stack_is_empty(error_stack)) {
    klv_destroy(klv);
    klv = NULL;
  }
  return klv;
}

// Takes ownership of the KWG
static inline KLV *klv_create_zeroed_from_kwg(KWG *kwg, int number_of_leaves,
                                              const char *klv_name) {
  KLV *klv = malloc_or_die(sizeof(KLV));
  klv->kwg = kwg;
  klv->name = string_duplicate(klv_name);
  klv->number_of_leaves = number_of_leaves;
  klv->leave_values = (Equity *)calloc_or_die(number_of_leaves, sizeof(Equity));
  const int number_of_kwg_nodes = kwg_get_number_of_nodes(kwg);
  klv->word_counts =
      (uint32_t *)calloc_or_die(number_of_kwg_nodes, sizeof(uint32_t));
  klv_count_words(klv, number_of_kwg_nodes);
  return klv;
}

static inline uint32_t klv_get_word_index_internal(const KLV *klv,
                                                   const Rack *leave,
                                                   uint32_t node_index) {
  int idx = 0;
  int lidx = 0;
  int lidx_letter_count = rack_get_letter(leave, lidx);
  int number_of_letters = rack_get_total_letters(leave);

  // Advance lidx
  while (lidx_letter_count == 0) {
    lidx++;
    lidx_letter_count = rack_get_letter(leave, lidx);
  }

  while (node_index != 0) {
    uint32_t next_word_index;
    node_index =
        increment_node_to_ml(klv, node_index, idx, &next_word_index, lidx);
    if (node_index == KLV_UNFOUND_INDEX) {
      return node_index;
    }
    idx = next_word_index;

    lidx_letter_count--;
    number_of_letters--;

    // Advance lidx
    while (lidx_letter_count == 0) {
      lidx++;
      if (lidx >= rack_get_dist_size(leave)) {
        break;
      }
      lidx_letter_count = rack_get_letter(leave, lidx);
    }

    if (number_of_letters == 0) {
      return idx;
    }

    node_index = follow_arc(klv, node_index, idx, &next_word_index);
    idx = next_word_index;
  }
  return KLV_UNFOUND_INDEX;
}

static inline int klv_get_word_index(const KLV *klv, const Rack *leave) {
  if (rack_is_empty(leave)) {
    return -1;
  }
  if (!klv) {
    return -1;
  }
  return klv_get_word_index_internal(klv, leave,
                                     kwg_get_dawg_root_node_index(klv->kwg));
}

static inline Equity klv_get_leave_value(const KLV *klv, const Rack *leave) {
  if (rack_is_empty(leave)) {
    return 0.0;
  }
  if (!klv) {
    return 0.0;
  }
  const uint32_t index = klv_get_word_index_internal(
      klv, leave, kwg_get_dawg_root_node_index(klv->kwg));
  return klv_get_indexed_leave_value(klv, index);
}

static inline void klv_write(const KLV *klv, const char *klv_filename,
                             ErrorStack *error_stack) {
  // Open the file stream for writing
  FILE *stream = fopen_safe(klv_filename, "wb", error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  const uint32_t kwg_number_of_nodes = kwg_get_number_of_nodes(klv->kwg);

  uint32_t kwg_size = htole32(kwg_number_of_nodes);
  fwrite_or_die(&kwg_size, sizeof(kwg_size), 1, stream, "kwg size");

  uint32_t *le_nodes =
      (uint32_t *)malloc_or_die(kwg_number_of_nodes * sizeof(uint32_t));
  for (uint32_t i = 0; i < kwg_number_of_nodes; i++) {
    le_nodes[i] = htole32(kwg_node(klv->kwg, i));
  }

  // Write the nodes to the stream
  fwrite_or_die(le_nodes, sizeof(uint32_t), kwg_number_of_nodes, stream,
                "kwg nodes");

  // Free the temporary buffer
  free(le_nodes);

  uint32_t number_of_leaves = klv->number_of_leaves;
  uint32_t le_number_of_leaves = htole32(number_of_leaves);
  fwrite_or_die(&le_number_of_leaves, sizeof(le_number_of_leaves), 1, stream,
                "number of leaves");

  float *le_floats = (float *)malloc_or_die(number_of_leaves * sizeof(float));
  for (uint32_t i = 0; i < number_of_leaves; i++) {
    const double leave_double = equity_to_double(klv->leave_values[i]);
    le_floats[i] = convert_float_to_le((float)leave_double);
  }
  fwrite_or_die(le_floats, sizeof(float), number_of_leaves, stream,
                "leave values");
  free(le_floats);

  fclose_or_die(stream);
}

#endif