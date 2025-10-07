#ifndef WMP_DEFS_H
#define WMP_DEFS_H

#include <stdint.h>

enum {
  WMP_INLINE_VALUE_BYTES = 16,
  WMP_NONINLINE_PADDING_BYTES =
      WMP_INLINE_VALUE_BYTES - 2 * sizeof(uint32_t) - 1,
  WMP_BITRACK_BYTES = 16,  // Store full 128-bit BitRack instead of quotient
  WMP_BUCKET_ITEMS_CAPACITY = 1,
  WMP_EARLIEST_SUPPORTED_VERSION = 3,  // Version 3 required for hash-based format
  WMP_VERSION = 3,  // Bumped version for new format
  WMP_RESULT_BUFFER_SIZE = 7000,
};

#endif
