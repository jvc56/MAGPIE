#ifndef KWG_H
#define KWG_H

#include "../compat/endian_conv.h"
#include "../def/kwg_defs.h"
#include "../util/fileproxy.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "data_filepaths.h"
#include "letter_distribution.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct KWG {
  char *name;
  uint32_t *nodes;
  int number_of_nodes;
} KWG;

// The KWG data structure was originally
// developed in wolges. For more details
// on how the KWG data structure works, see
// https://github.com/andy-k/wolges/blob/main/details.txt

static inline const char *kwg_get_name(const KWG *kwg) { return kwg->name; }

static inline int kwg_get_number_of_nodes(const KWG *kwg) {
  return kwg->number_of_nodes;
}

static inline uint32_t kwg_node(const KWG *kwg, uint32_t node_index) {
  return kwg->nodes[node_index];
}

static inline bool kwg_node_is_end(uint32_t node) {
  return (node & KWG_NODE_IS_END_FLAG) != 0;
}

static inline bool kwg_node_accepts(uint32_t node) {
  return (node & KWG_NODE_ACCEPTS_FLAG) != 0;
}

static inline uint32_t kwg_node_arc_index(uint32_t node) {
  return (node & KWG_ARC_INDEX_MASK);
}

static inline uint32_t kwg_node_arc_index_prefetch(uint32_t node,
                                                   const KWG *kwg) {
  const uint32_t next_node = (node & KWG_ARC_INDEX_MASK);
#if defined(__has_builtin) && __has_builtin(__builtin_prefetch)
  __builtin_prefetch(&kwg->nodes[next_node]);
#endif
  return next_node;
}

static inline uint32_t kwg_node_tile(uint32_t node) {
  return node >> KWG_TILE_BIT_OFFSET;
}

static inline uint32_t kwg_get_dawg_root_node_index(const KWG *kwg) {
  const uint32_t dawg_pointer_node = kwg_node(kwg, 0);
  return kwg_node_arc_index(dawg_pointer_node);
}

static inline uint32_t kwg_get_root_node_index(const KWG *kwg) {
  const uint32_t gaddag_pointer_node = kwg_node(kwg, 1);
  return kwg_node_arc_index(gaddag_pointer_node);
}

static inline uint32_t kwg_get_next_node_index(const KWG *kwg,
                                               uint32_t node_index,
                                               MachineLetter letter) {
  uint32_t i = node_index;
  while (1) {
    const uint32_t node = kwg_node(kwg, i);
    if (kwg_node_tile(node) == letter) {
      return kwg_node_arc_index_prefetch(node, kwg);
    }
    if (kwg_node_is_end(node)) {
      return 0;
    }
    i++;
  }
}

static inline uint64_t kwg_get_letter_sets(const KWG *kwg, uint32_t node_index,
                                           uint64_t *extension_set) {
  uint64_t ls = 0;
  uint64_t es = 0;
  for (uint32_t i = node_index;; ++i) {
    const uint32_t node = kwg_node(kwg, i);
    const uint32_t t = kwg_node_tile(node);
    const uint64_t bit = ((uint64_t)1 << t) ^ (!t);
    es |= bit;
    ls |= bit & (uint64_t)-(int64_t)kwg_node_accepts(node);
    if (kwg_node_is_end(node)) {
      break;
    }
  }
  *extension_set = es;
  return ls;
}

static inline void kwg_allocate_nodes(KWG *kwg, size_t number_of_nodes) {
  kwg->nodes = (uint32_t *)malloc_or_die(number_of_nodes * sizeof(uint32_t));
  kwg->number_of_nodes = (int)number_of_nodes;
}

static inline void kwg_read_nodes_from_stream(KWG *kwg, size_t number_of_nodes,
                                              FILE *stream) {
  kwg_allocate_nodes(kwg, number_of_nodes);
  size_t result = fread(kwg->nodes, sizeof(uint32_t), number_of_nodes, stream);
  if (result != number_of_nodes) {
    log_fatal("kwg nodes fread failure: %zd != %zd", result, number_of_nodes);
  }
  for (uint32_t i = 0; i < number_of_nodes; i++) {
    kwg->nodes[i] = le32toh(kwg->nodes[i]);
  }
}

static inline uint32_t *kwg_get_mutable_nodes(const KWG *kwg) {
  return kwg->nodes;
}

static inline void load_kwg(const char *kwg_name, const char *kwg_filename,
                            KWG *kwg, ErrorStack *error_stack) {
  FILE *stream = stream_from_filename(kwg_filename, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  kwg->name = string_duplicate(kwg_name);

  fseek_or_die(stream, 0, SEEK_END); // seek to end of file
  long int kwg_size = ftell(stream); // get current file pointer
  fseek_or_die(stream, 0, SEEK_SET);

  size_t number_of_nodes = kwg_size / sizeof(uint32_t);

  kwg_read_nodes_from_stream(kwg, number_of_nodes, stream);

  fclose_or_die(stream);
}

static inline void kwg_destroy(KWG *kwg) {
  if (!kwg) {
    return;
  }
  free(kwg->nodes);
  free(kwg->name);
  free(kwg);
}

static inline KWG *kwg_create(const char *data_paths, const char *kwg_name,
                              ErrorStack *error_stack) {
  char *kwg_filename = data_filepaths_get_readable_filename(
      data_paths, kwg_name, DATA_FILEPATH_TYPE_KWG, error_stack);
  KWG *kwg = NULL;
  if (error_stack_is_empty(error_stack)) {
    kwg = calloc_or_die(1, sizeof(KWG));
    load_kwg(kwg_name, kwg_filename, kwg, error_stack);
  }
  free(kwg_filename);
  if (!error_stack_is_empty(error_stack)) {
    kwg_destroy(kwg);
    kwg = NULL;
  }
  return kwg;
}

static inline KWG *kwg_create_empty(void) {
  KWG *kwg = malloc_or_die(sizeof(KWG));
  kwg->name = NULL;
  return kwg;
}

static inline void kwg_write_to_file(const KWG *kwg, const char *filename,
                                     ErrorStack *error_stack) {
  FILE *stream = fopen_safe(filename, "wb", error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  for (int i = 0; i < kwg->number_of_nodes; i++) {
    const uint32_t node = htole32(kwg->nodes[i]);
    fwrite_or_die(&node, sizeof(uint32_t), 1, stream, "kwg node");
  }
  fclose_or_die(stream);
}

static inline bool kwg_in_letter_set(const KWG *kwg, MachineLetter letter,
                                     uint32_t node_index) {
  letter = get_unblanked_machine_letter(letter);
  uint32_t i = node_index;
  for (;;) {
    const uint32_t node = kwg_node(kwg, i);
    if (kwg_node_tile(node) == letter) {
      return kwg_node_accepts(node);
    }
    if (kwg_node_is_end(node)) {
      return false;
    }
    i++;
  }
}

#endif