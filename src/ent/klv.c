#include "klv.h"

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

#include "kwg.h"
#include "rack.h"

char *klv_get_filepath(const char *klv_name) {
  // Check for invalid inputs
  if (!klv_name) {
    log_fatal("klv name is null");
  }
  return get_formatted_string("%s%s%s", KLV_FILEPATH, klv_name,
                              KLV_FILE_EXTENSION);
}

float reverse_float(const float in_float) {
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

// Egregious hack to convert endianness of a float
float convert_little_endian_to_host(const float little_endian_float) {
  // Check if host machine is little-endian
  union {
    uint32_t i;
    char c[4];
  } endian_check = {0x01020304};

  if (endian_check.c[0] == 4) {
    // Host machine is little-endian
    return little_endian_float;
  } else {
    // Host machine is big-endian
    return reverse_float(little_endian_float);
  }
}

int klv_count_words_at(const KLV *klv, uint32_t node_index, uint32_t kwg_size) {
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

void klv_count_words(const KLV *klv, size_t kwg_size) {
  for (int p = kwg_size - 1; p >= 0; p--) {
    klv_count_words_at(klv, p, (int)kwg_size);
  }
}

void klv_load(KLV *klv, const char *klv_name) {
  char *klv_filename = klv_get_filepath(klv_name);
  FILE *stream = stream_from_filename(klv_filename);
  if (!stream) {
    log_fatal("failed to open stream from filename: %s\n", klv_filename);
  }
  free(klv_filename);

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

  klv->leave_values =
      (double *)malloc_or_die(number_of_leaves * sizeof(double));
  float *temp_floats = (float *)malloc_or_die(number_of_leaves * sizeof(float));
  result = fread(temp_floats, sizeof(float), number_of_leaves, stream);
  if (result != number_of_leaves) {
    log_fatal("edges fread failure: %zd != %d\n", result, number_of_leaves);
  }

  fclose(stream);

  for (uint32_t i = 0; i < number_of_leaves; i++) {
    klv->leave_values[i] =
        (double)convert_little_endian_to_host(temp_floats[i]);
  }
  free(temp_floats);

  klv->word_counts = malloc_or_die(kwg_size * sizeof(uint32_t));
  for (size_t i = 0; i < kwg_size; i++) {
    klv->word_counts[i] = 0;
  }

  klv_count_words(klv, kwg_size);
}

KLV *klv_create(const char *klv_name) {
  KLV *klv = malloc_or_die(sizeof(KLV));
  klv_load(klv, klv_name);
  return klv;
}

void klv_destroy(KLV *klv) {
  if (!klv) {
    return;
  }
  kwg_destroy(klv->kwg);
  free(klv->leave_values);
  free(klv->word_counts);
  free(klv);
}

uint32_t klv_get_word_index_of(const KLV *klv, const Rack *leave,
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

double klv_get_leave_value(const KLV *klv, const Rack *leave) {
  if (rack_is_empty(leave)) {
    return 0.0;
  }
  if (!klv) {
    return 0.0;
  }
  const uint32_t index =
      klv_get_word_index_of(klv, leave, kwg_get_dawg_root_node_index(klv->kwg));
  return klv_get_indexed_leave_value(klv, index);
}
