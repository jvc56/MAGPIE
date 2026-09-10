#ifndef WORD_INFO_TABLE_H
#define WORD_INFO_TABLE_H

#include "../def/board_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../util/io_util.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

// WordInfoTable (parallel to RackInfoTable): one trie per key-word length. At a
// length-`len` word's terminal node sits a value row of (BOARD_DIM - len + 1)
// bitvectors. Entry i is the union of letters outside each occurrence of
// that substring in any length-(len + i) word. Repeated letters outside the
// fixed substring remain available. Older version-3 files include the fixed
// letters too; those broader masks remain conservative.
//
// Splitting by length lets each trie's value table use a stride sized to that
// length: a length-`len` key can only be a substring of words of length
// `len..BOARD_DIM`, so the always-empty rows below `len` are never stored.
//
// Each trie is a flat array of nodes laid out so a node's siblings are
// contiguous (scan until node_last), and node_child gives the first child.
// Node 0 is the null node. A word-terminal node has node_value >= 0, an index
// into `values`. Looking a block up traverses the trie for its length (the
// block is always a real word on a legal board); a phony (or non-word path)
// returns NULL and the caller permits all letters.
enum {
  // Bump WIT_VERSION whenever the on-disk layout changes incompatibly.
  WIT_VERSION = 4,
  WIT_FLAG_WORD_PLUS_FLOATER = 1,
  WIT_EARLIEST_SUPPORTED_VERSION = 3,
  WPF_MIN_BLOCK_LENGTH = 2,
  WPF_MAX_BLOCK_LENGTH = 4,
  WPF_ALPHABET_SIZE = 26,
  WIT_POSITION_MIN_BASE_LENGTH = 2,
  WIT_POSITION_MAX_BASE_LENGTH = BOARD_DIM,
};

static_assert(BOARD_DIM < 32, "WIT positional lengths must fit in uint32_t");

typedef struct WitTrie {
  uint32_t num_nodes;
  uint8_t *node_tile;   // [num_nodes]
  uint8_t *node_last;   // [num_nodes] 1 if last sibling
  uint32_t *node_child; // [num_nodes] first child node index (0 = none)
  int32_t *node_value;  // [num_nodes] value index, or -1
  uint32_t root;        // first top-level node index (0 if empty)
  uint32_t num_values;
  uint32_t *values; // [num_values * stride], stride = BOARD_DIM - len + 1
} WitTrie;

typedef struct WordInfoTable {
  char *name;
  uint8_t version;
  // kwg_get_hash of the KWG this table was built from, so loading pairs it
  // with the matching graph. Zero for a table built from a bare word list,
  // which has nothing to verify against.
  uint64_t kwg_hash;
  // Indexed by key-word length; tries[0] is unused, tries[len] holds the
  // length-`len` words. Lengths run 1..BOARD_DIM.
  WitTrie tries[BOARD_DIM + 1];
  // Optional positional table, indexed directly by this WIT's value IDs.
  uint32_t *word_plus_floater[BOARD_DIM + 1];
  // Derived from this table's complete dictionary on construction/load.
  // For each value ID, position p contains bits for absolute final lengths
  // having this base at p. It does not change the serialized WIT format.
  uint32_t *position_lengths[BOARD_DIM + 1];
} WordInfoTable;

static inline size_t word_plus_floater_cells_per_key(int block_length) {
  const size_t extension = (size_t)(BOARD_DIM - block_length);
  return WPF_ALPHABET_SIZE * extension * (extension + 1);
}

// Number of value slots for a key of length `len`: total word lengths run
// len..BOARD_DIM inclusive.
static inline int wit_stride_for_len(int len) { return BOARD_DIM - len + 1; }

static inline const char *word_info_table_get_name(const WordInfoTable *wit) {
  return wit->name;
}

static inline uint64_t word_info_table_get_kwg_hash(const WordInfoTable *wit) {
  return wit->kwg_hash;
}

// Returns the value row for `block` (a word of length `len`), or NULL if
// `block` is not a real word of that length. The row has
// wit_stride_for_len(len) entries; entry i is the union of letters outside
// each occurrence of `block` in length-(len + i) words.
static inline const uint32_t *word_info_table_lookup(const WordInfoTable *wit,
                                                     const MachineLetter *block,
                                                     int len) {
  if (len < 1 || len > BOARD_DIM) {
    return NULL;
  }
  const WitTrie *trie = &wit->tries[len];
  uint32_t node = trie->root;
  if (node == 0) {
    return NULL;
  }
  for (int pos = 0; pos < len; pos++) {
    const MachineLetter tile = block[pos];
    for (;;) {
      if (trie->node_tile[node] == tile) {
        break;
      }
      if (trie->node_last[node]) {
        return NULL;
      }
      node++;
    }
    if (pos == len - 1) {
      const int32_t value_index = trie->node_value[node];
      return value_index < 0
                 ? NULL
                 : trie->values +
                       (size_t)value_index * (size_t)wit_stride_for_len(len);
    }
    node = trie->node_child[node];
    if (node == 0) {
      return NULL;
    }
  }
  return NULL;
}

// Return the derived row only when the borrowed ordinary row belongs to
// this exact table. Shared-KWG board lanes can borrow the other player's WIT.
static inline const uint32_t *word_info_table_get_position_lengths(
    const WordInfoTable *wit, const uint32_t *ordinary_row, int base_length) {
  if (wit == NULL || ordinary_row == NULL ||
      base_length < WIT_POSITION_MIN_BASE_LENGTH ||
      base_length > WIT_POSITION_MAX_BASE_LENGTH ||
      wit->position_lengths[base_length] == NULL) {
    return NULL;
  }
  const uintptr_t address = (uintptr_t)ordinary_row;
  const uintptr_t values = (uintptr_t)wit->tries[base_length].values;
  const size_t stride = (size_t)wit_stride_for_len(base_length);
  const size_t row_bytes = stride * sizeof(uint32_t);
  if (address < values) {
    return NULL;
  }
  const uintptr_t offset = address - values;
  if (offset >= (size_t)wit->tries[base_length].num_values * row_bytes ||
      offset % row_bytes != 0) {
    return NULL;
  }
  return wit->position_lengths[base_length] + ((offset / row_bytes) * stride);
}

static inline void word_info_table_clear_position_lengths(WordInfoTable *wit) {
  for (int length = 0; length <= BOARD_DIM; length++) {
    free(wit->position_lengths[length]);
    wit->position_lengths[length] = NULL;
  }
}

// Requires a complete validated WIT dictionary, as produced by the native
// maker or loader. A zero source fingerprint leaves this optional cache absent.
void word_info_table_build_position_lengths(WordInfoTable *wit);

static inline void word_info_table_destroy(WordInfoTable *wit) {
  if (wit == NULL) {
    return;
  }
  for (int len = 0; len <= BOARD_DIM; len++) {
    WitTrie *trie = &wit->tries[len];
    free(trie->node_tile);
    free(trie->node_last);
    free(trie->node_child);
    free(trie->node_value);
    free(trie->values);
    free(wit->word_plus_floater[len]);
    free(wit->position_lengths[len]);
  }
  free(wit->name);
  free(wit);
}

// Positional tables are optional for legacy WITs and unsupported alphabets.
static inline void word_info_table_clear_word_plus_floater(WordInfoTable *wit) {
  for (int length = 0; length <= BOARD_DIM; length++) {
    free(wit->word_plus_floater[length]);
    wit->word_plus_floater[length] = NULL;
  }
}

// Version 4 keeps the version-3 trie representation and appends optional
// positional masks. Both use one KWG identity and the same terminal IDs.
void word_info_table_write_to_file(const WordInfoTable *wit,
                                   const char *filename,
                                   ErrorStack *error_stack);
void word_info_table_load(WordInfoTable *wit, const char *name,
                          const char *filename, ErrorStack *error_stack);
WordInfoTable *word_info_table_create(const char *data_paths,
                                      const char *wit_name,
                                      ErrorStack *error_stack);

#endif
