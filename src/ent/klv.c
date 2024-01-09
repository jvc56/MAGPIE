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

#include "kwg.h"
#include "rack.h"

#include "../util/fileproxy.h"
#include "../util/log.h"
#include "../util/string_util.h"
#include "../util/util.h"

// The KLV data structure was originally
// developed in wolges. For more details
// on how the KLV data structure works, see
// https://github.com/andy-k/wolges/blob/main/details.txt
struct KLV {
  KWG *kwg;
  int *word_counts;
  float *leave_values;
};

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

int klv_count_words_at(const KLV *klv, int p, int kwg_size) {
  if (p >= kwg_size) {
    return 0;
  }
  if (klv->word_counts[p] == -1) {
    log_fatal("unexpected -1 at %d\n", p);
  }
  if (klv->word_counts[p] == 0) {
    klv->word_counts[p] = -1;

    int a = 0;
    if (kwg_accepts(klv->kwg, p)) {
      a = 1;
    }
    int b = 0;
    int arc_index_p = kwg_arc_index(klv->kwg, p);
    if (arc_index_p != 0) {
      b = klv_count_words_at(klv, arc_index_p, kwg_size);
    }
    int c = 0;
    bool is_not_end = !kwg_is_end(klv->kwg, p);
    if (is_not_end) {
      c = klv_count_words_at(klv, p + 1, kwg_size);
    }
    klv->word_counts[p] = a + b + c;
  }
  return klv->word_counts[p];
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

  klv->leave_values = (float *)malloc_or_die(number_of_leaves * sizeof(float));
  result = fread(klv->leave_values, sizeof(float), number_of_leaves, stream);
  if (result != number_of_leaves) {
    log_fatal("edges fread failure: %zd != %d\n", result, number_of_leaves);
  }

  fclose(stream);

  for (uint32_t i = 0; i < number_of_leaves; i++) {
    klv->leave_values[i] = convert_little_endian_to_host(klv->leave_values[i]);
  }

  klv->word_counts = (int *)malloc_or_die(kwg_size * sizeof(int));
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

int klv_get_word_index_of(const KLV *klv, const Rack *leave,
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
    idx += klv->word_counts[node_index];
    while (kwg_tile(klv->kwg, node_index) != (uint8_t)lidx) {
      if (kwg_is_end(klv->kwg, node_index)) {
        return -1;
      }
      node_index++;
    }
    idx -= klv->word_counts[node_index];

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
      if (kwg_accepts(klv->kwg, node_index)) {
        return idx;
      }
      return -1;
    }
    if (kwg_accepts(klv->kwg, node_index)) {
      idx += 1;
    }
    node_index = kwg_arc_index(klv->kwg, node_index);
  }
  return -1;
}

double klv_get_leave_value(const KLV *klv, const Rack *leave) {
  if (rack_is_empty(leave)) {
    return 0.0;
  }
  if (!klv) {
    return 0.0;
  }
  int index = klv_get_word_index_of(klv, leave, kwg_arc_index(klv->kwg, 0));
  if (index != -1) {
    return (double)klv->leave_values[index];
  }
  return 0.0;
}