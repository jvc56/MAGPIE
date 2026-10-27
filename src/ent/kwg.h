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
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct KWG {
  char *name;
  uint32_t *nodes;
  int number_of_nodes;
  // For each node, the tiles of it and of the siblings after it in its list,
  // as a bitset; see kwg_try_build_tile_bitsets. Every lexicon has them,
  // loaded or made from words in alphabetical order. A leave graph may not,
  // and its KLV reads only its nodes.
  uint64_t *sibling_tiles;
  // The same, for only the nodes that accept: those tiles end a word.
  uint64_t *accepting_tiles;
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

static_assert(MAX_ALPHABET_SIZE <= 64,
              "every tile must fit in a word graph's 64-bit tile bitsets");

// The node in the list from node_index on whose tile is tile_bit, given the
// list's sibling tiles (see kwg_try_build_tile_bitsets), which have it: the
// list's tiles ascend, so it follows one node for each tile below its own.
static inline uint32_t kwg_list_node(const KWG *kwg, uint32_t node_index,
                                     uint64_t list_tiles, uint64_t tile_bit) {
  return kwg_node(kwg, node_index + (uint32_t)kwg_bit_count_64(list_tiles &
                                                               (tile_bit - 1)));
}

static inline uint32_t kwg_get_next_node_index(const KWG *kwg,
                                               uint32_t node_index,
                                               MachineLetter letter) {
  assert(kwg->sibling_tiles != NULL);
  const uint64_t list_tiles = kwg->sibling_tiles[node_index];
  const uint64_t tile_bit = (uint64_t)1 << letter;
  if ((list_tiles & tile_bit) == 0) {
    return 0;
  }
  return kwg_node_arc_index_prefetch(
      kwg_list_node(kwg, node_index, list_tiles, tile_bit), kwg);
}

// Builds the KWG's tile bitsets once its nodes are in place, so a list's arc
// for a tile is found without walking the list (kwg_list_node), and its letter
// sets are read off (kwg_get_letter_sets). That needs every list's tiles to
// ascend, as lexicons are written; for a KWG where they don't, nothing is
// built and false is returned. Leave graphs may not ascend: the rack list
// makes its own from racks in generation order, and some KLV files' don't.
// Only their KLV reads them, through their nodes.
static inline bool kwg_try_build_tile_bitsets(KWG *kwg) {
  const int number_of_nodes = kwg->number_of_nodes;
  uint64_t *sibling_tiles =
      malloc_or_die((size_t)number_of_nodes * sizeof(uint64_t));
  uint64_t *accepting_tiles =
      malloc_or_die((size_t)number_of_nodes * sizeof(uint64_t));
  uint64_t after = 0;
  uint64_t accepting_after = 0;
  uint32_t next_tile = 0;
  for (int node_idx = number_of_nodes - 1; node_idx >= 0; node_idx--) {
    const uint32_t node = kwg->nodes[node_idx];
    const uint32_t tile = kwg_node_tile(node);
    const bool is_end = kwg_node_is_end(node);
    if (tile >= 64 || (!is_end && tile >= next_tile)) {
      free(sibling_tiles);
      free(accepting_tiles);
      return false;
    }
    if (is_end) {
      after = 0;
      accepting_after = 0;
    }
    const uint64_t tile_bit = (uint64_t)1 << tile;
    after |= tile_bit;
    accepting_after |= tile_bit & (uint64_t)-(int64_t)kwg_node_accepts(node);
    sibling_tiles[node_idx] = after;
    accepting_tiles[node_idx] = accepting_after;
    next_tile = tile;
  }
  free(kwg->sibling_tiles);
  free(kwg->accepting_tiles);
  kwg->sibling_tiles = sibling_tiles;
  kwg->accepting_tiles = accepting_tiles;
  return true;
}

static inline const uint64_t *kwg_get_sibling_tiles(const KWG *kwg) {
  assert(kwg->sibling_tiles != NULL);
  return kwg->sibling_tiles;
}

// The tiles of the list from node_index on that end a word, and in
// *extension_set all of them; both leave out the separator, tile 0.
static inline uint64_t kwg_get_letter_sets(const KWG *kwg, uint32_t node_index,
                                           uint64_t *extension_set) {
  assert(kwg->sibling_tiles != NULL);
  *extension_set = kwg->sibling_tiles[node_index] & ~(uint64_t)1;
  return kwg->accepting_tiles[node_index] & ~(uint64_t)1;
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
  if (!kwg_try_build_tile_bitsets(kwg)) {
    log_fatal("lexicon %s has a node list whose tiles don't ascend", kwg_name);
  }

  fclose_or_die(stream);
}

static inline void kwg_destroy(KWG *kwg) {
  if (!kwg) {
    return;
  }
  free(kwg->nodes);
  free(kwg->name);
  free(kwg->sibling_tiles);
  free(kwg->accepting_tiles);
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
  kwg->accepting_tiles = NULL;
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
  assert(kwg->accepting_tiles != NULL);
  return (kwg->accepting_tiles[node_index] &
          ((uint64_t)1 << get_unblanked_machine_letter(letter))) != 0;
}

#endif