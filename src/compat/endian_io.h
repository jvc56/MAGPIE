#ifndef ENDIAN_IO_H
#define ENDIAN_IO_H

#include "endian_conv.h"
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Transfer little-endian uint32 arrays without changing the write buffer.
// Empty arrays succeed without accessing the buffer or stream. Callers own
// the stream and handle short transfers; these helpers never close it.
static inline bool fwrite_le_uint32s(const uint32_t *values, size_t count,
                                     FILE *stream) {
  if (count == 0) {
    return true;
  }
#if IS_LITTLE_ENDIAN
  return fwrite(values, sizeof(uint32_t), count, stream) == count;
#else
  for (size_t index = 0; index < count; index++) {
    const uint32_t little_endian_value = htole32(values[index]);
    if (fwrite(&little_endian_value, sizeof(uint32_t), 1, stream) != 1) {
      return false;
    }
  }
  return true;
#endif
}

static inline bool fread_le_uint32s(uint32_t *values, size_t count,
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

// IEEE binary32 payloads, including NaNs, are preserved bit for bit.
static inline bool fwrite_le_floats(const float *values, size_t count,
                                    FILE *stream) {
  static_assert(sizeof(float) == sizeof(uint32_t), "float must be 32 bits");
  if (count == 0) {
    return true;
  }
#if IS_LITTLE_ENDIAN
  return fwrite(values, sizeof(float), count, stream) == count;
#else
  for (size_t index = 0; index < count; index++) {
    uint32_t bits;
    memcpy(&bits, &values[index], sizeof(bits));
    if (!fwrite_le_uint32s(&bits, 1, stream)) {
      return false;
    }
  }
  return true;
#endif
}

#endif
