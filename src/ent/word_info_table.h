#ifndef WORD_INFO_TABLE_H
#define WORD_INFO_TABLE_H

#include "../compat/endian_conv.h"
#include "../def/board_defs.h"
#include "../util/fileproxy.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "data_filepaths.h"
#include "letter_distribution.h"
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
  WIT_VERSION = 3,
  WIT_EARLIEST_SUPPORTED_VERSION = 3,
  WPF_MIN_BLOCK_LENGTH = 2,
  WPF_MAX_BLOCK_LENGTH = 4,
  WPF_ALPHABET_SIZE = 26,
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
  // kwg_get_hash of the KWG this table was built from, so loading pairs it
  // with the matching graph. Zero for a table built from a bare word list,
  // which has nothing to verify against.
  uint64_t kwg_hash;
  // Indexed by key-word length; tries[0] is unused, tries[len] holds the
  // length-`len` words. Lengths run 1..BOARD_DIM.
  WitTrie tries[BOARD_DIM + 1];
  // Optional positional table, indexed by this WIT's value
  // IDs for dense payloads; sparse payloads remap through
  // word_plus_floater_ids.
  uint32_t *word_plus_floater[BOARD_DIM + 1];
  // NULL means dense value IDs. Otherwise -1 marks an uncovered base; a
  // nonnegative ID addresses a covered row, even when every mask is zero.
  int32_t *word_plus_floater_ids[BOARD_DIM + 1];
} WordInfoTable;

static inline size_t word_plus_floater_cells_per_key(int block_length) {
  const size_t extension = (size_t)(BOARD_DIM - block_length);
  return WPF_ALPHABET_SIZE * extension * (extension + 1);
}

static inline uint64_t wpf_hash_uint32(uint64_t hash, uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    hash = (hash ^ ((value >> shift) & 255U)) * 1099511628211ULL;
  }
  return hash;
}

static inline uint64_t word_plus_floater_layout_hash(const WordInfoTable *wit) {
  uint64_t hash = 1469598103934665603ULL;
  for (int length = WPF_MIN_BLOCK_LENGTH; length <= WPF_MAX_BLOCK_LENGTH;
       length++) {
    const WitTrie *trie = &wit->tries[length];
    hash = wpf_hash_uint32(hash, (uint32_t)length);
    hash = wpf_hash_uint32(hash, trie->num_nodes);
    hash = wpf_hash_uint32(hash, trie->root);
    hash = wpf_hash_uint32(hash, trie->num_values);
    for (uint32_t index = 0; index < trie->num_nodes; index++) {
      hash = (hash ^ trie->node_tile[index]) * 1099511628211ULL;
    }
    for (uint32_t index = 0; index < trie->num_nodes; index++) {
      hash = (hash ^ trie->node_last[index]) * 1099511628211ULL;
    }
    for (uint32_t index = 0; index < trie->num_nodes; index++) {
      hash = wpf_hash_uint32(hash, trie->node_child[index]);
    }
    for (uint32_t index = 0; index < trie->num_nodes; index++) {
      hash = wpf_hash_uint32(hash, (uint32_t)trie->node_value[index]);
    }
  }
  return hash;
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
    free(wit->word_plus_floater_ids[len]);
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
//   8 bytes: kwg_hash (uint64, 0 if unknown)
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
  const uint64_t kwg_hash_le = htole64(wit->kwg_hash);
  fwrite_or_die(&kwg_hash_le, sizeof(kwg_hash_le), 1, stream, "wit kwg hash");
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

// M1 stores dense value-ID rows over its inclusive key-length interval. M2
// stores an int32 ID for each original WIT value (-1 is uncovered), then only
// covered rows. Both bind to the complete 2..4 WIT key layout. An absent or
// foreign-lexicon table permits all letters; a covered zero row is exact.
static inline void word_info_table_clear_word_plus_floater(WordInfoTable *wit) {
  for (int length = 0; length <= BOARD_DIM; length++) {
    free(wit->word_plus_floater[length]);
    free(wit->word_plus_floater_ids[length]);
    wit->word_plus_floater[length] = NULL;
    wit->word_plus_floater_ids[length] = NULL;
  }
}

static inline bool wpf_read_uint32s(uint32_t *values, size_t count,
                                    FILE *stream) {
  if (count != 0 && fread(values, sizeof(uint32_t), count, stream) != count) {
    return false;
  }
#if !IS_LITTLE_ENDIAN
  for (size_t index = 0; index < count; index++) {
    values[index] = le32toh(values[index]);
  }
#endif
  return true;
}

static inline void
word_info_table_read_word_plus_floater(WordInfoTable *wit, FILE *stream,
                                       ErrorStack *error_stack) {
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  word_info_table_clear_word_plus_floater(wit);
  const char *error_message = "truncated WordPlusFloater header";
  uint8_t *seen = NULL;
  const unsigned char dense_magic[8] = {'W', 'P', 'F', 'M', '1', 'L', 'E', 0};
  const unsigned char sparse_magic[8] = {'W', 'P', 'F', 'M', '2', 'L', 'E', 0};
  unsigned char magic[8];
  if (fread(magic, 1, sizeof(magic), stream) != sizeof(magic)) {
    goto invalid;
  }
  const bool sparse = memcmp(magic, sparse_magic, sizeof(magic)) == 0;
  if (!sparse && memcmp(magic, dense_magic, sizeof(magic)) != 0) {
    error_message = "invalid WordPlusFloater header";
    goto invalid;
  }
  uint32_t header[4];
  uint64_t hash;
  uint64_t layout_hash;
  if (!wpf_read_uint32s(header, 4, stream) ||
      fread(&hash, sizeof(hash), 1, stream) != 1 ||
      fread(&layout_hash, sizeof(layout_hash), 1, stream) != 1) {
    goto invalid;
  }
  hash = le64toh(hash);
  layout_hash = le64toh(layout_hash);
  if (header[0] != BOARD_DIM || hash != wit->kwg_hash || hash == 0) {
    return;
  }
  const uint32_t minimum_length = header[1];
  const uint32_t maximum_length = header[2];
  if (minimum_length < WPF_MIN_BLOCK_LENGTH ||
      maximum_length > WPF_MAX_BLOCK_LENGTH ||
      minimum_length > maximum_length || header[3] != 0) {
    error_message = "unsupported WordPlusFloater coverage";
    goto invalid;
  }
  if (layout_hash != word_plus_floater_layout_hash(wit)) {
    error_message = "WordPlusFloater key IDs do not match this WIT";
    goto invalid;
  }
  for (uint32_t length = minimum_length; length <= maximum_length; length++) {
    uint32_t section[4];
    error_message = "truncated WordPlusFloater section";
    if (!wpf_read_uint32s(section, 4, stream)) {
      goto invalid;
    }
    const uint32_t num_values = section[1];
    const uint32_t cells_per_key = section[2];
    uint32_t stored_values = section[3];
    if (section[0] != length || num_values != wit->tries[length].num_values ||
        cells_per_key != word_plus_floater_cells_per_key((int)length) ||
        (!sparse && stored_values != 0)) {
      error_message = "WordPlusFloater section does not match WIT key layout";
      goto invalid;
    }
    if (!sparse) {
      stored_values = num_values;
    }
    if (stored_values > num_values || stored_values > INT32_MAX ||
        stored_values > SIZE_MAX / cells_per_key ||
        (size_t)stored_values * cells_per_key > SIZE_MAX / sizeof(uint32_t) ||
        (uint64_t)num_values * sizeof(int32_t) > SIZE_MAX) {
      error_message = "WordPlusFloater allocation would overflow";
      goto invalid;
    }
    if (sparse) {
      int32_t *ids = num_values != 0
                         ? malloc_or_die((size_t)num_values * sizeof(int32_t))
                         : NULL;
      wit->word_plus_floater_ids[length] = ids;
      error_message = "truncated WordPlusFloater row IDs";
      if (!wpf_read_uint32s((uint32_t *)ids, num_values, stream)) {
        goto invalid;
      }
      seen = stored_values != 0 ? calloc_or_die(stored_values, 1) : NULL;
      uint32_t covered = 0;
      for (uint32_t index = 0; index < num_values; index++) {
        const int32_t row_id = ids[index];
        if (row_id == -1) {
          continue;
        }
        if (row_id < 0 || (uint32_t)row_id >= stored_values || seen[row_id]) {
          error_message = "invalid WordPlusFloater row ID";
          goto invalid;
        }
        seen[row_id] = 1;
        covered++;
      }
      free(seen);
      seen = NULL;
      if (covered != stored_values) {
        error_message = "incomplete WordPlusFloater row ID coverage";
        goto invalid;
      }
    }
    const size_t count = (size_t)stored_values * cells_per_key;
    wit->word_plus_floater[length] =
        count != 0 ? malloc_or_die(count * sizeof(uint32_t)) : NULL;
    error_message = "truncated WordPlusFloater masks";
    if (!wpf_read_uint32s(wit->word_plus_floater[length], count, stream)) {
      goto invalid;
    }
  }
  if (fgetc(stream) != EOF || ferror(stream)) {
    error_message = "unexpected data after WordPlusFloater table";
    goto invalid;
  }
  return;

invalid:
  free(seen);
  word_info_table_clear_word_plus_floater(wit);
  error_stack_push(error_stack, ERROR_STATUS_RW_READ_ERROR,
                   string_duplicate(error_message));
}

static inline void
word_info_table_load_word_plus_floater(WordInfoTable *wit,
                                       const char *data_paths, const char *name,
                                       ErrorStack *error_stack) {
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  word_info_table_clear_word_plus_floater(wit);
  char *filename = data_filepaths_get_readable_filename(
      data_paths, name, DATA_FILEPATH_TYPE_WORD_PLUS_FLOATER, error_stack);
  if (filename == NULL) {
    if (error_stack_top(error_stack) == ERROR_STATUS_FILEPATH_FILE_NOT_FOUND) {
      error_stack_reset(error_stack);
    }
    return;
  }
  FILE *stream = stream_from_filename(filename, error_stack);
  if (error_stack_is_empty(error_stack)) {
    word_info_table_read_word_plus_floater(wit, stream, error_stack);
    fclose_or_die(stream);
  }
  free(filename);
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
  uint64_t kwg_hash_le;
  if (fread(&kwg_hash_le, sizeof(kwg_hash_le), 1, stream) != 1) {
    log_fatal("could not read wit kwg hash");
  }
  wit->kwg_hash = le64toh(kwg_hash_le);

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
    if (error_stack_is_empty(error_stack)) {
      word_info_table_load_word_plus_floater(wit, data_paths, wit_name,
                                             error_stack);
    }
  }
  free(wit_filename);
  if (!error_stack_is_empty(error_stack)) {
    word_info_table_destroy(wit);
    wit = NULL;
  }
  return wit;
}

#endif
