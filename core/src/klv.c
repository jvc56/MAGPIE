#if defined(__APPLE__)
#include "../compat/endian.h"
#else
#include <endian.h>
#endif
#include <stdio.h>
#include <stdlib.h>

#include "fileproxy.h"
#include "klv.h"
#include "log.h"
#include "rack.h"
#include "util.h"

int count_words_at(KLV *klv, int p, int kwg_size) {
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
      b = count_words_at(klv, arc_index_p, kwg_size);
    }
    int c = 0;
    int is_not_end = !kwg_is_end(klv->kwg, p);
    if (is_not_end) {
      c = count_words_at(klv, p + 1, kwg_size);
    }
    klv->word_counts[p] = a + b + c;
  }
  return klv->word_counts[p];
}

void count_words(KLV *klv, size_t kwg_size) {
  for (int p = kwg_size - 1; p >= 0; p--) {
    count_words_at(klv, p, (int)kwg_size);
  }
}

float reverse_float(const float in_float) {
  float ret_val;
  char *float_to_convert = (char *)&in_float;
  char *return_float = (char *)&ret_val;

  // swap the bytes into a temporary buffer
  return_float[0] = float_to_convert[3];
  return_float[1] = float_to_convert[2];
  return_float[2] = float_to_convert[1];
  return_float[3] = float_to_convert[0];

  return ret_val;
}

// Egregious hack to convert endianness of a float
float convert_little_endian_to_host(float little_endian_float) {
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

void load_klv(KLV *klv, const char *klv_filename) {
  FILE *stream = stream_from_filename(klv_filename);
  if (!stream) {
    log_fatal("failed to open stream from filename: %s\n", klv_filename);
  }

  uint32_t kwg_size;
  size_t result;

  result = fread(&kwg_size, sizeof(kwg_size), 1, stream);
  if (result != 1) {
    log_fatal("kwg size fread failure: %zd != %d\n", result, 1);
  }
  kwg_size = le32toh(kwg_size);

  klv->kwg = malloc_or_die(sizeof(KWG));
  klv->kwg->nodes = (uint32_t *)malloc_or_die(kwg_size * sizeof(uint32_t));
  result = fread(klv->kwg->nodes, sizeof(uint32_t), kwg_size, stream);
  if (result != kwg_size) {
    log_fatal("kwg nodes fread failure: %zd != %d\n", result, kwg_size);
  }
  for (uint32_t i = 0; i < kwg_size; i++) {
    klv->kwg->nodes[i] = le32toh(klv->kwg->nodes[i]);
  }

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

  count_words(klv, kwg_size);
}

KLV *create_klv(const char *klv_filename) {
  KLV *klv = malloc_or_die(sizeof(KLV));
  load_klv(klv, klv_filename);
  return klv;
}

void destroy_klv(KLV *klv) {
  destroy_kwg(klv->kwg);
  free(klv->leave_values);
  free(klv->word_counts);
  free(klv);
}

int get_word_index_of(KLV *klv, int32_t node_index, Rack *leave) {
  int idx = 0;
  int lidx = 0;
  int lidx_letter_count = leave->array[lidx];
  int number_of_letters = leave->number_of_letters;

  // Advance lidx
  while (lidx_letter_count == 0) {
    lidx++;
    lidx_letter_count = leave->array[lidx];
  }

  while (node_index != 0) {
    int next_word_index;
    node_index =
        increment_node_to_ml(klv, node_index, idx, &next_word_index, lidx);
    if (node_index == -1) {
      return node_index;
    }
    idx = next_word_index;

    lidx_letter_count--;
    number_of_letters--;

    // Advance lidx
    while (lidx_letter_count == 0) {
      lidx++;
      if (lidx >= leave->array_size) {
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
  return -1;
}

double get_leave_value(KLV *klv, Rack *leave) {
  if (leave->empty) {
    return 0.0;
  }
  if (!klv) {
    return 0.0;
  }
  int index = get_word_index_of(klv, kwg_arc_index(klv->kwg, 0), leave);
  if (index != -1) {
    return (double)klv->leave_values[index];
  }
  return 0.0;
}

int32_t increment_node_to_ml(KLV *klv, int32_t node_index, int32_t word_index,
                             int *next_word_index, uint8_t ml) {
  *next_word_index = word_index;
  while (kwg_tile(klv->kwg, node_index) != ml) {
    if (kwg_is_end(klv->kwg, node_index)) {
      break;
    }
    *next_word_index +=
        klv->word_counts[node_index] - klv->word_counts[node_index + 1];
    node_index++;
  }
  return node_index;
}

int32_t follow_arc(KLV *klv, int32_t node_index, int32_t word_index,
                   int32_t *next_word_index) {
  *next_word_index = word_index + 1;
  return kwg_arc_index(klv->kwg, node_index);
}