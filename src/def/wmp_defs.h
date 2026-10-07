#ifndef WMP_DEFS_H
#define WMP_DEFS_H

#include <stdint.h>

enum {
  WMP_INLINE_VALUE_BYTES = 16,
  WMP_NONINLINE_PADDING_BYTES =
      WMP_INLINE_VALUE_BYTES - 2 * sizeof(uint32_t) - 1,
  WMP_BITRACK_BYTES = 16,
  WMP_BUCKET_ITEMS_CAPACITY = 1,
  WMP_EARLIEST_SUPPORTED_VERSION = 3,
  WMP_VERSION = 3,
  WMP_RESULT_BUFFER_SIZE = 7000,
  // The blankless lookup index stores the entries in buckets of
  // WMP_INDEX_BUCKET_SLOTS, one 128-byte cache line on Apple silicon.
  WMP_INDEX_BUCKET_SLOTS = 4,
  WMP_INDEX_BUCKET_ALIGNMENT = 128,
  WMP_INDEX_MIN_BUCKETS = 16,
  // At most this percent of the index's slots hold entries.
  WMP_INDEX_MAX_FILL_PERCENT = 75,
  // Miss filter: bits per entry, and the width of a bit index into one
  // 64-bit filter word.
  WMP_FILTER_BITS_PER_ENTRY = 16,
  WMP_FILTER_BIT_INDEX_BITS = 6,
  WMP_FILTER_MIN_WORDS = 2,
};

// Odd multipliers for wmp_index_hash.
#define WMP_INDEX_HASH_LOW_MULTIPLIER 0x9E3779B97F4A7C15ULL
#define WMP_INDEX_HASH_HIGH_MULTIPLIER 0xC2B2AE3D27D4EB4FULL

#endif
