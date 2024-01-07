#if defined(__APPLE__)
#include "../compat/endian.h"
#else
#include <endian.h>
#endif
#include <stdio.h>
#include <stdlib.h>

#include "fileproxy.h"
#include "kwg.h"
#include "letter_distribution.h"
#include "log.h"
#include "util.h"

char *get_kwg_filepath(const char *kwg_name) {
  // Check for invalid inputs
  if (!kwg_name) {
    log_fatal("kwg name is null");
  }
  return get_formatted_string("%s%s%s", KWG_FILEPATH, kwg_name,
                              KWG_FILE_EXTENSION);
}

void load_kwg(KWG *kwg, const char *kwg_name) {
  char *kwg_filename = get_kwg_filepath(kwg_name);

  FILE *stream = stream_from_filename(kwg_filename);
  if (!stream) {
    log_fatal("failed to open stream from filename: %s\n", kwg_filename);
  }
  free(kwg_filename);

  fseek(stream, 0, SEEK_END);        // seek to end of file
  long int kwg_size = ftell(stream); // get current file pointer
  fseek(stream, 0, SEEK_SET);

  size_t number_of_nodes = kwg_size / sizeof(uint32_t);

  size_t result;

  kwg->nodes = (uint32_t *)malloc_or_die(number_of_nodes * sizeof(uint32_t));
  result = fread(kwg->nodes, sizeof(uint32_t), number_of_nodes, stream);
  if (result != number_of_nodes) {
    log_fatal("kwg nodes fread failure: %zd != %zd", result, number_of_nodes);
  }
  for (uint32_t i = 0; i < number_of_nodes; i++) {
    kwg->nodes[i] = le32toh(kwg->nodes[i]);
  }
  fclose(stream);
}

KWG *create_kwg(const char *kwg_name) {
  KWG *kwg = malloc_or_die(sizeof(KWG));
  load_kwg(kwg, kwg_name);
  return kwg;
}

void destroy_kwg(KWG *kwg) {
  free(kwg->nodes);
  free(kwg);
}

extern inline bool kwg_is_end(const KWG *kwg, int node_index);
extern inline bool kwg_accepts(const KWG *kwg, int node_index);
extern inline int kwg_arc_index(const KWG *kwg, int node_index);
extern inline int kwg_tile(const KWG *kwg, int node_index);
extern inline int kwg_get_dawg_root_node_index(const KWG *kwg);
extern inline int kwg_get_root_node_index(const KWG *kwg);

int kwg_get_next_node_index(const KWG *kwg, int node_index, int letter) {
  int i = node_index;
  while (1) {
    if (kwg_tile(kwg, i) == letter) {
      return kwg_arc_index(kwg, i);
    }
    if (kwg_is_end(kwg, i)) {
      return 0;
    }
    i++;
  }
}

bool kwg_in_letter_set(const KWG *kwg, int letter, int node_index) {
  letter = get_unblanked_machine_letter(letter);
  int i = node_index;
  while (1) {
    if (kwg_tile(kwg, i) == letter) {
      return kwg_accepts(kwg, i);
    }
    if (kwg_is_end(kwg, i)) {
      return false;
    }
    i++;
  }
}

uint64_t kwg_get_letter_set(const KWG *kwg, int node_index) {
  uint64_t ls = 0;
  int i = node_index;
  while (1) {
    int t = kwg_tile(kwg, i);
    if (kwg_accepts(kwg, i)) {
      ls |= ((uint64_t)1 << t);
    }
    if (kwg_is_end(kwg, i)) {
      break;
    }
    i++;
  }
  return ls;
}
