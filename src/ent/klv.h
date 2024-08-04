#ifndef KLV_H
#define KLV_H

#if defined(__APPLE__)
#include "../../compat/endian.h"
#else
#include <endian.h>
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../def/klv_defs.h"

#include "../util/fileproxy.h"
#include "../util/log.h"
#include "../util/string_util.h"
#include "../util/util.h"

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
  double *leave_values;
} KLV;

static inline const char *klv_get_name(const KLV *klv) { return klv->name; }

static inline const KWG *klv_get_kwg(const KLV *klv) { return klv->kwg; }

static inline int klv_get_number_of_leaves(const KLV *klv) {
  return klv->number_of_leaves;
}

static inline void klv_set_all_leave_values_to_zero(KLV *klv) {
  memset(klv->leave_values, 0, klv->number_of_leaves * sizeof(double));
}

static inline double klv_get_indexed_leave_value(const KLV *klv,
                                                 uint32_t index) {
  if (index == KLV_UNFOUND_INDEX) {
    return 0.0;
  }
  return klv->leave_values[index];
}

// Assumes the index is valid
static inline double klv_set_indexed_leave_value(const KLV *klv, uint32_t index,
                                                 double value) {
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

// Returns the float in little-endian format.
// - If the host is little-endian, it returns the original float
// - If the host is not little-endian, it must therefore be big-endian
//   and returns the reverse of the original float to convert it to
//   little-endian format.
static inline float convert_float_to_le(const float input_float) {
  // Check if host machine is little-endian
  union {
    uint32_t i;
    char c[4];
  } endian_check = {0x01020304};

  if (endian_check.c[0] == 4) {
    // Host machine is little-endian
    return input_float;
  } else {
    // Host machine is big-endian
    return reverse_float(input_float);
  }
}

static inline int klv_count_words_at(const KLV *klv, uint32_t node_index,
                                     uint32_t kwg_size) {
  if (node_index >= kwg_size) {
    return 0;
  }
  if (klv->word_counts[node_index] == KLV_UNFOUND_INDEX) {
    log_fatal("unexpected KLV_EMPTY_NODE_WORD_COUNT at %d\n", node_index);
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

static inline void klv_load(KLV *klv, const char *data_paths,
                            const char *klv_name) {
  char *klv_filename = data_filepaths_get_readable_filename(
      data_paths, klv_name, DATA_FILEPATH_TYPE_KLV);

  FILE *stream = stream_from_filename(klv_filename);
  if (!stream) {
    log_fatal("failed to open stream from filename: %s\n", klv_filename);
  }
  free(klv_filename);

  klv->name = string_duplicate(klv_name);

  uint32_t kwg_size;
  size_t result;

  result = fread(&kwg_size, sizeof(kwg_size), 1, stream);
  if (result != 1) {
    log_fatal("kwg size fread failure: %zd != %d\n", result, 1);
  }
  kwg_size = le32toh(kwg_size);

  klv->kwg = kwg_create_empty();

  kwg_read_nodes_from_stream(klv->kwg, kwg_size, stream);

  uint32_t number_of_leaves;
  result = fread(&number_of_leaves, sizeof(number_of_leaves), 1, stream);
  if (result != 1) {
    log_fatal("number of leaves fread failure: %zd != %d\n", result, 1);
  }
  number_of_leaves = le32toh(number_of_leaves);
  klv->number_of_leaves = number_of_leaves;

  klv->leave_values =
      (double *)malloc_or_die(number_of_leaves * sizeof(double));
  float *temp_floats = (float *)malloc_or_die(number_of_leaves * sizeof(float));
  result = fread(temp_floats, sizeof(float), number_of_leaves, stream);
  if (result != number_of_leaves) {
    log_fatal("edges fread failure: %zd != %d\n", result, number_of_leaves);
  }

  fclose(stream);

  for (uint32_t i = 0; i < number_of_leaves; i++) {
    klv->leave_values[i] = (double)convert_float_to_le(temp_floats[i]);
  }
  free(temp_floats);

  klv->word_counts = malloc_or_die(kwg_size * sizeof(uint32_t));
  for (size_t i = 0; i < kwg_size; i++) {
    klv->word_counts[i] = 0;
  }

  klv_count_words(klv, kwg_size);
}

static inline KLV *klv_create(const char *data_paths, const char *klv_name) {
  KLV *klv = malloc_or_die(sizeof(KLV));
  klv->name = NULL;
  klv_load(klv, data_paths, klv_name);
  return klv;
}

// Takes ownership of the KWG
static inline KLV *klv_create_zeroed_from_kwg(KWG *kwg, int number_of_leaves,
                                              const char *klv_name) {
  KLV *klv = malloc_or_die(sizeof(KLV));
  klv->kwg = kwg;
  klv->name = string_duplicate(klv_name);
  klv->number_of_leaves = number_of_leaves;
  klv->leave_values = (double *)calloc_or_die(number_of_leaves, sizeof(double));
  const int number_of_kwg_nodes = kwg_get_number_of_nodes(kwg);
  klv->word_counts =
      (uint32_t *)calloc_or_die(number_of_kwg_nodes, sizeof(uint32_t));
  klv_count_words(klv, number_of_kwg_nodes);
  return klv;
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
      // FIXME: this should use the rack methods
      lidx++;
      if (lidx >= leave->dist_size) {
        break;
      }
      lidx_letter_count = leave->array[lidx];
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

static inline double klv_get_leave_value(const KLV *klv, const Rack *leave) {
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

static inline void klv_write(const KLV *klv, const char *data_path,
                             const char *full_filepath) {
  char *klv_filename = NULL;
  if (full_filepath) {
    klv_filename = string_duplicate(full_filepath);
  } else {
    klv_filename = data_filepaths_get_writable_filename(data_path, klv->name,
                                                        DATA_FILEPATH_TYPE_KLV);
  }
  // Open the file stream for writing
  FILE *stream = fopen(klv_filename, "wb");
  if (!stream) {
    log_fatal("failed to open stream for writing: %s\n", klv_filename);
  }
  free(klv_filename);

  const uint32_t kwg_number_of_nodes = kwg_get_number_of_nodes(klv->kwg);

  uint32_t kwg_size = htole32(kwg_number_of_nodes);
  size_t result = fwrite(&kwg_size, sizeof(kwg_size), 1, stream);
  if (result != 1) {
    log_fatal("kwg size fwrite failure: %zd != %d\n", result, 1);
  }

  uint32_t *le_nodes =
      (uint32_t *)malloc_or_die(kwg_number_of_nodes * sizeof(uint32_t));
  for (uint32_t i = 0; i < kwg_number_of_nodes; i++) {
    le_nodes[i] = htole32(kwg_node(klv->kwg, i));
  }

  // Write the nodes to the stream
  result = fwrite(le_nodes, sizeof(uint32_t), kwg_number_of_nodes, stream);
  if (result != kwg_number_of_nodes) {
    log_fatal("kwg nodes fwrite failure: %zd != %zd", result,
              kwg_number_of_nodes);
  }

  // Free the temporary buffer
  free(le_nodes);

  uint32_t number_of_leaves = klv->number_of_leaves;
  uint32_t le_number_of_leaves = htole32(number_of_leaves);
  result = fwrite(&le_number_of_leaves, sizeof(le_number_of_leaves), 1, stream);
  if (result != 1) {
    log_fatal("number of leaves fwrite failure: %zd != %d\n", result, 1);
  }

  float *le_floats = (float *)malloc_or_die(number_of_leaves * sizeof(float));
  for (uint32_t i = 0; i < number_of_leaves; i++) {
    le_floats[i] = convert_float_to_le((float)klv->leave_values[i]);
  }
  result = fwrite(le_floats, sizeof(float), number_of_leaves, stream);
  if (result != number_of_leaves) {
    log_fatal("leave values fwrite failure: %zd != %d\n", result,
              number_of_leaves);
  }
  free(le_floats);

  fclose(stream);
}

#endif