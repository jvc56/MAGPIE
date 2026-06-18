#ifndef WORD_INFO_TABLE_H
#define WORD_INFO_TABLE_H

#include "../compat/endian_conv.h"
#include "../def/board_defs.h"
#include "../util/fileproxy.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "data_filepaths.h"
#include "letter_distribution.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// WordInfoTable (parallel to RackInfoTable): one trie per key-word length. At a
// length-`len` word's terminal node sits a value row of (BOARD_DIM - len + 1)
// bitvectors. Entry i is the union of the letters appearing in any
// length-(len + i) word that contains that word as a contiguous substring.
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
  WIT_VERSION = 2,
  WIT_EARLIEST_SUPPORTED_VERSION = 2,
};

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
  // Indexed by key-word length; tries[0] is unused, tries[len] holds the
  // length-`len` words. Lengths run 1..BOARD_DIM.
  WitTrie tries[BOARD_DIM + 1];
} WordInfoTable;

// Number of value slots for a key of length `len`: total word lengths run
// len..BOARD_DIM inclusive.
static inline int wit_stride_for_len(int len) { return BOARD_DIM - len + 1; }

static inline const char *word_info_table_get_name(const WordInfoTable *wit) {
  return wit->name;
}

// Returns the value row for `block` (a word of length `len`), or NULL if
// `block` is not a real word of that length. The row has
// wit_stride_for_len(len) entries; entry i is the union of letter-sets of all
// length-(len + i) words that contain `block` as a contiguous substring.
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
  }
  free(wit->name);
  free(wit);
}

// ============================================================================
// File I/O
// ============================================================================
//
// On-disk layout (all integers little-endian):
//   1 byte:  version (WIT_VERSION)
//   1 byte:  board_dim (matches BOARD_DIM)
//   2 bytes: zero padding (keeps the following u32s aligned)
//   For each key length len = 1..BOARD_DIM (in order), one trie:
//     4 bytes: num_nodes
//     4 bytes: root
//     4 bytes: num_values
//     num_nodes bytes:      node_tile (uint8)
//     num_nodes bytes:      node_last (uint8)
//     num_nodes * 4 bytes:  node_child (uint32)
//     num_nodes * 4 bytes:  node_value (int32)
//     num_values * stride * 4 bytes: values (uint32), stride = BOARD_DIM-len+1

static inline void wit_write_uint32_or_die(uint32_t value, FILE *stream,
                                           const char *description) {
  const uint32_t le = htole32(value);
  fwrite_or_die(&le, sizeof(le), 1, stream, description);
}

static inline void wit_write_uint32s_or_die(const uint32_t *values, size_t n,
                                            FILE *stream,
                                            const char *description) {
#if IS_LITTLE_ENDIAN
  fwrite_or_die(values, sizeof(uint32_t), n, stream, description);
#else
  for (size_t i = 0; i < n; i++) {
    const uint32_t le = htole32(values[i]);
    fwrite_or_die(&le, sizeof(uint32_t), 1, stream, description);
  }
#endif
}

static inline void wit_write_trie_or_die(const WitTrie *trie, int stride,
                                         FILE *stream) {
  wit_write_uint32_or_die(trie->num_nodes, stream, "wit num nodes");
  wit_write_uint32_or_die(trie->root, stream, "wit root");
  wit_write_uint32_or_die(trie->num_values, stream, "wit num values");
  fwrite_or_die(trie->node_tile, sizeof(uint8_t), trie->num_nodes, stream,
                "wit node tile");
  fwrite_or_die(trie->node_last, sizeof(uint8_t), trie->num_nodes, stream,
                "wit node last");
  wit_write_uint32s_or_die(trie->node_child, trie->num_nodes, stream,
                           "wit node child");
  wit_write_uint32s_or_die((const uint32_t *)trie->node_value, trie->num_nodes,
                           stream, "wit node value");
  wit_write_uint32s_or_die(trie->values,
                           (size_t)trie->num_values * (size_t)stride, stream,
                           "wit values");
}

static inline void word_info_table_write_to_file(const WordInfoTable *wit,
                                                 const char *filename,
                                                 ErrorStack *error_stack) {
  FILE *stream = fopen_safe(filename, "wb", error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  const uint8_t version = wit->version;
  fwrite_or_die(&version, sizeof(version), 1, stream, "wit version");
  const uint8_t board_dim = (uint8_t)BOARD_DIM;
  fwrite_or_die(&board_dim, sizeof(board_dim), 1, stream, "wit board dim");
  const uint8_t padding[2] = {0, 0};
  fwrite_or_die(padding, sizeof(uint8_t), 2, stream, "wit header padding");
  for (int len = 1; len <= BOARD_DIM; len++) {
    wit_write_trie_or_die(&wit->tries[len], wit_stride_for_len(len), stream);
  }
  fclose_or_die(stream);
}

static inline void wit_read_uint32_or_die(uint32_t *out, FILE *stream) {
  if (fread(out, sizeof(uint32_t), 1, stream) != 1) {
    log_fatal("could not read uint32 from wit stream");
  }
  *out = le32toh(*out);
}

static inline void wit_read_uint32s_or_die(uint32_t *out, size_t n,
                                           FILE *stream) {
  if (n > 0 && fread(out, sizeof(uint32_t), n, stream) != n) {
    log_fatal("could not read uint32s from wit stream");
  }
#if !IS_LITTLE_ENDIAN
  for (size_t i = 0; i < n; i++) {
    out[i] = le32toh(out[i]);
  }
#endif
}

static inline void wit_read_trie_or_die(WitTrie *trie, int stride,
                                        FILE *stream) {
  wit_read_uint32_or_die(&trie->num_nodes, stream);
  wit_read_uint32_or_die(&trie->root, stream);
  wit_read_uint32_or_die(&trie->num_values, stream);
  trie->node_tile = (uint8_t *)malloc_or_die(trie->num_nodes * sizeof(uint8_t));
  if (trie->num_nodes > 0 &&
      fread(trie->node_tile, sizeof(uint8_t), trie->num_nodes, stream) !=
          trie->num_nodes) {
    log_fatal("could not read wit node tile");
  }
  trie->node_last = (uint8_t *)malloc_or_die(trie->num_nodes * sizeof(uint8_t));
  if (trie->num_nodes > 0 &&
      fread(trie->node_last, sizeof(uint8_t), trie->num_nodes, stream) !=
          trie->num_nodes) {
    log_fatal("could not read wit node last");
  }
  trie->node_child =
      (uint32_t *)malloc_or_die(trie->num_nodes * sizeof(uint32_t));
  wit_read_uint32s_or_die(trie->node_child, trie->num_nodes, stream);
  trie->node_value =
      (int32_t *)malloc_or_die(trie->num_nodes * sizeof(int32_t));
  wit_read_uint32s_or_die((uint32_t *)trie->node_value, trie->num_nodes,
                          stream);
  const size_t num_value_words = (size_t)trie->num_values * (size_t)stride;
  trie->values =
      num_value_words > 0
          ? (uint32_t *)malloc_or_die(num_value_words * sizeof(uint32_t))
          : NULL;
  wit_read_uint32s_or_die(trie->values, num_value_words, stream);
}

static inline void word_info_table_load(WordInfoTable *wit, const char *name,
                                        const char *filename,
                                        ErrorStack *error_stack) {
  FILE *stream = stream_from_filename(filename, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  uint8_t version;
  if (fread(&version, sizeof(version), 1, stream) != 1) {
    log_fatal("could not read wit version");
  }
  if (version < WIT_EARLIEST_SUPPORTED_VERSION) {
    error_stack_push(
        error_stack, ERROR_STATUS_WMP_UNSUPPORTED_VERSION,
        get_formatted_string(
            "detected wit version %d but only %d or greater is supported: %s\n",
            version, WIT_EARLIEST_SUPPORTED_VERSION, filename));
    fclose_or_die(stream);
    return;
  }
  wit->version = version;

  uint8_t board_dim;
  if (fread(&board_dim, sizeof(board_dim), 1, stream) != 1) {
    log_fatal("could not read wit board dim");
  }
  if (board_dim != (uint8_t)BOARD_DIM) {
    error_stack_push(
        error_stack, ERROR_STATUS_WMP_INCOMPATIBLE_BOARD_DIM,
        get_formatted_string(
            "wit board dim %d does not match build board dim %d: %s\n",
            board_dim, BOARD_DIM, filename));
    fclose_or_die(stream);
    return;
  }
  uint8_t padding[2];
  if (fread(padding, sizeof(uint8_t), 2, stream) != 2) {
    log_fatal("could not read wit header padding");
  }

  for (int len = 1; len <= BOARD_DIM; len++) {
    wit_read_trie_or_die(&wit->tries[len], wit_stride_for_len(len), stream);
  }
  fclose_or_die(stream);
  wit->name = string_duplicate(name);
}

static inline WordInfoTable *word_info_table_create(const char *data_paths,
                                                    const char *wit_name,
                                                    ErrorStack *error_stack) {
  char *wit_filename = data_filepaths_get_readable_filename(
      data_paths, wit_name, DATA_FILEPATH_TYPE_WORD_INFO_TABLE, error_stack);
  WordInfoTable *wit = NULL;
  if (error_stack_is_empty(error_stack)) {
    wit = (WordInfoTable *)calloc_or_die(1, sizeof(WordInfoTable));
    word_info_table_load(wit, wit_name, wit_filename, error_stack);
  }
  free(wit_filename);
  if (!error_stack_is_empty(error_stack)) {
    word_info_table_destroy(wit);
    wit = NULL;
  }
  return wit;
}

#endif
