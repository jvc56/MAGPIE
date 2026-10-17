#ifndef WMP_DEFS_H
#define WMP_DEFS_H

#include "../compat/ccache.h"
#include "board_defs.h"
#include <stdint.h>

enum {
  WMP_INLINE_VALUE_BYTES = 16,
  WMP_NONINLINE_PADDING_BYTES =
      WMP_INLINE_VALUE_BYTES - 2 * sizeof(uint32_t) - 1,
  // The inline slot table covers every word length an entry can be read at
  // and every length a word slot can have.
  WMP_INLINE_SLOT_TABLE_SIZE =
      (BOARD_DIM > WMP_INLINE_VALUE_BYTES ? BOARD_DIM
                                          : WMP_INLINE_VALUE_BYTES) +
      1,
  // An inlined byte count times the table's inverse length, shifted right by
  // this, is the word count.
  WMP_INLINE_INVERSE_LENGTH_SHIFT = 8,
  WMP_BITRACK_BYTES = 16,
  WMP_BUCKET_ITEMS_CAPACITY = 1,
  WMP_EARLIEST_SUPPORTED_VERSION = 3,
  WMP_VERSION = 3,
  WMP_RESULT_BUFFER_SIZE = 7000,
  // A blank expansion copies each inlined word list with one
  // WMP_INLINE_VALUE_BYTES move, so the word writers can store fewer than
  // this many bytes past the end of the words they return. Every buffer
  // passed to them needs this much room beyond the longest result
  // (max_word_lookup_bytes, expected to be at most WMP_RESULT_BUFFER_SIZE).
  WMP_WORD_BUFFER_SLACK_BYTES = WMP_INLINE_VALUE_BYTES,
  WMP_INDEX_MIN_BUCKETS = 16,
  // At most this percent of the index's slots hold entries.
  WMP_INDEX_MAX_FILL_PERCENT = 75,
  // Miss filter: bits per entry, and the width of a bit index into one
  // 64-bit filter word.
  WMP_FILTER_BITS_PER_ENTRY = 16,
  WMP_FILTER_BIT_INDEX_BITS = 6,
  WMP_FILTER_MIN_WORDS = 2,
};

// The blankless lookup index stores the entries in buckets of
// WMP_INDEX_BUCKET_SLOTS 32-byte entries, one cache line: 128 bytes on Apple
// silicon, 64 bytes elsewhere.
enum {
  WMP_INDEX_ENTRY_BYTES = 32,
  WMP_INDEX_BUCKET_SLOTS = CCACHE_LINE_BYTES / WMP_INDEX_ENTRY_BYTES,
  WMP_INDEX_BUCKET_ALIGNMENT = CCACHE_LINE_BYTES,
};

// Odd multipliers for wmp_index_hash.
#define WMP_INDEX_HASH_LOW_MULTIPLIER 0x9E3779B97F4A7C15ULL
#define WMP_INDEX_HASH_HIGH_MULTIPLIER 0xC2B2AE3D27D4EB4FULL

typedef enum {
  WMP_BUILD_WORD_LIST,
  WMP_BUILD_WORDS,
  WMP_BUILD_BLANKS,
  WMP_BUILD_DOUBLE_BLANKS,
  WMP_BUILD_INDEXES,
} wmp_build_stage_t;

#endif
