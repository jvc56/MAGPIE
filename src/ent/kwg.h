#ifndef KWG_H
#define KWG_H

#include "../compat/cfile.h"
#include "../compat/endian_conv.h"
#include "../def/kwg_defs.h"
#include "../util/fileproxy.h"
#include "../util/fnv.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "data_filepaths.h"
#include "letter_distribution.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct KWG {
  char *name;
  uint32_t *nodes;
  int number_of_nodes;
  // For each node, the tiles of it and of the siblings after it in its list,
  // as a bitset; see kwg_build_sibling_tiles. NULL unless built.
  uint64_t *sibling_tiles;
} KWG;

// The KWG data structure was originally
// developed in wolges. For more details
// on how the KWG data structure works, see
// https://github.com/andy-k/wolges/blob/main/details.txt

static inline const char *kwg_get_name(const KWG *kwg) { return kwg->name; }

static inline int kwg_get_number_of_nodes(const KWG *kwg) {
  return kwg->number_of_nodes;
}

// FNV-1a over the node array. Identifies the exact word graph a derived table
// was built from, so a table built from a different one fails to pair with
// it instead of silently misdescribing the words on the board.
static inline uint64_t kwg_get_hash(const KWG *kwg) {
  uint64_t hash = FNV_64_OFFSET_BASIS;
  hash = fnv64a_step(hash, (uint64_t)kwg->number_of_nodes);
  for (int node_idx = 0; node_idx < kwg->number_of_nodes; node_idx++) {
    hash = fnv64a_step(hash, (uint64_t)kwg->nodes[node_idx]);
  }
  return hash;
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

// The number of set bits, following the guarded-intrinsic pattern.
static inline int kwg_bit_count_64(uint64_t bitset) {
#if defined(__has_builtin) && __has_builtin(__builtin_popcountll)
  return __builtin_popcountll(bitset);
#else
  int count = 0;
  while (bitset != 0) {
    bitset &= bitset - 1;
    count++;
  }
  return count;
#endif
}

// The node for tile in the list that starts at node_index, whose sibling
// tiles (see kwg_build_sibling_tiles) are list_tiles and include the tile.
static inline uint32_t kwg_sibling_node_index(uint32_t node_index,
                                              uint64_t list_tiles,
                                              MachineLetter tile) {
  return node_index +
         (uint32_t)kwg_bit_count_64(list_tiles & (((uint64_t)1 << tile) - 1));
}

// The tile's bit in a list's sibling tiles, or 0 for a tile too large for
// any list to hold.
static inline uint64_t kwg_sibling_tile_bit(MachineLetter tile) {
  return tile < 64 ? (uint64_t)1 << tile : 0;
}

static inline uint32_t kwg_get_next_node_index(const KWG *kwg,
                                               uint32_t node_index,
                                               MachineLetter letter) {
  if (kwg->sibling_tiles != NULL) {
    const uint64_t list_tiles = kwg->sibling_tiles[node_index];
    if ((list_tiles & kwg_sibling_tile_bit(letter)) == 0) {
      return 0;
    }
    return kwg_node_arc_index_prefetch(
        kwg_node(kwg, kwg_sibling_node_index(node_index, list_tiles, letter)),
        kwg);
  }
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

// Builds kwg->sibling_tiles, so a node list's arc for a tile is found from
// the list's first node without walking the list: it is there exactly when
// the list's bitset has the tile, at the first node plus the number of the
// list's tiles below it. That needs every list's tiles to ascend and to fit in
// the bitset; for a KWG where they don't, nothing is built.
static inline void kwg_build_sibling_tiles(KWG *kwg) {
  const int number_of_nodes = kwg->number_of_nodes;
  uint64_t *sibling_tiles =
      malloc_or_die((size_t)number_of_nodes * sizeof(uint64_t));
  uint64_t after = 0;
  uint32_t next_tile = 0;
  for (int node_idx = number_of_nodes - 1; node_idx >= 0; node_idx--) {
    const uint32_t node = kwg->nodes[node_idx];
    const uint32_t tile = kwg_node_tile(node);
    if (kwg_node_is_end(node)) {
      after = 0;
    } else if (tile >= next_tile) {
      free(sibling_tiles);
      return;
    }
    if (tile >= 64) {
      free(sibling_tiles);
      return;
    }
    after |= (uint64_t)1 << tile;
    sibling_tiles[node_idx] = after;
    next_tile = tile;
  }
  free(kwg->sibling_tiles);
  kwg->sibling_tiles = sibling_tiles;
}

static inline const uint64_t *kwg_get_sibling_tiles(const KWG *kwg) {
  return kwg->sibling_tiles;
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

  int64_t kwg_size = 0;
  if (!cfile_stream_size(stream, &kwg_size)) {
    log_fatal("failed to get the size of %s", kwg_filename);
  }

  size_t number_of_nodes = (size_t)kwg_size / sizeof(uint32_t);

  kwg_read_nodes_from_stream(kwg, number_of_nodes, stream);
  kwg_build_sibling_tiles(kwg);

  fclose_or_die(stream);
}

static inline void kwg_destroy(KWG *kwg) {
  if (!kwg) {
    return;
  }
  free(kwg->nodes);
  free(kwg->name);
  free(kwg->sibling_tiles);
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
  kwg->sibling_tiles = NULL;
  return kwg;
}

static inline void kwg_write_to_file(const KWG *kwg, const char *filename,
                                     ErrorStack *error_stack) {
  FILE *stream = fopen_safe(filename, "wb", error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
#if IS_LITTLE_ENDIAN
  // On little-endian systems, write the entire array at once
  fwrite_or_die(kwg->nodes, sizeof(uint32_t), kwg->number_of_nodes, stream,
                "kwg nodes");
#else
  // On big-endian systems, convert each node
  for (int i = 0; i < kwg->number_of_nodes; i++) {
    const uint32_t node = htole32(kwg->nodes[i]);
    fwrite_or_die(&node, sizeof(uint32_t), 1, stream, "kwg node");
  }
#endif
  fclose_or_die(stream);
}

static inline bool kwg_in_letter_set(const KWG *kwg, MachineLetter letter,
                                     uint32_t node_index) {
  letter = get_unblanked_machine_letter(letter);
  if (kwg->sibling_tiles != NULL) {
    const uint64_t list_tiles = kwg->sibling_tiles[node_index];
    if ((list_tiles & kwg_sibling_tile_bit(letter)) == 0) {
      return false;
    }
    return kwg_node_accepts(
        kwg_node(kwg, kwg_sibling_node_index(node_index, list_tiles, letter)));
  }
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