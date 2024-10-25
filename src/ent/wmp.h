#ifndef WMP_H
#define WMP_H

#if defined(__APPLE__)
#include "../../compat/endian.h"
#else
#include <endian.h>
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../def/board_defs.h"
#include "../def/wmp_defs.h"

#include "../util/fileproxy.h"
#include "../util/log.h"
#include "../util/string_util.h"
#include "../util/util.h"

#include "data_filepaths.h"

typedef struct WMPEntry {
  uint8_t bucket_or_inline[WMP_INLINE_VALUE_BYTES];
  uint8_t quotient[WMP_QUOTIENT_BYTES];
} WMPEntry;

typedef struct WMPForLength {
  uint32_t num_words;
  uint32_t num_word_buckets;
  uint32_t num_blank_buckets;
  uint32_t num_double_blank_buckets;
  uint32_t num_blank_pairs;
  uint32_t num_double_blank_entries;
  uint32_t *word_bucket_starts;
  uint32_t *blank_bucket_starts;
  uint32_t *double_blank_bucket_starts;
  uint32_t *double_blank_letters;
  WMPEntry *word_map_entries;
  WMPEntry *blank_map_entries;
  WMPEntry *double_blank_map_entries;
} WMPForLength;

typedef struct WMP {
  char *name;
  uint8_t major_version;
  uint8_t minor_version;
  uint8_t min_word_length;
  uint8_t max_word_length;
  uint32_t max_blank_pair_bytes;
  uint32_t max_word_lookup_bytes;
  WMPForLength maps[BOARD_DIM + 1];
} WMP;

static inline bool read_byte_from_stream(uint8_t *byte, FILE *stream) {
  const size_t result = fread(byte, sizeof(uint8_t), 1, stream);
  if (result != 1) {
    log_fatal("could not read byte from stream\n");
    return false;
  }
  return true;
}

static inline uint32_t read_uint32_from_stream(uint32_t *i, FILE *stream) {
  const size_t result = fread(i, sizeof(uint32_t), 1, stream);
  if (result != 1) {
    log_fatal("could not read uint32 from stream\n");
    return false;
  }
  return true;
}

static inline bool read_header_from_stream(WMP *wmp, FILE *stream) {
  if (!read_byte_from_stream(&wmp->major_version, stream)) { return false; }
  if (!read_byte_from_stream(&wmp->minor_version, stream)) { return false; }
  if (!read_byte_from_stream(&wmp->min_word_length, stream)) { return false; }
  if (!read_byte_from_stream(&wmp->max_word_length, stream)) { return false; }
  if (!read_uint32_from_stream(&wmp->max_word_lookup_bytes, stream)) {
    return false;
  }
  if (!read_uint32_from_stream(&wmp->max_blank_pair_bytes, stream)) {
    return false;
  }
  return true;
}

static inline void wmp_load(WMP *wmp, const char *data_paths,
                            const char *wmp_name) {
  char *wmp_filename = data_filepaths_get_readable_filename(
      data_paths, wmp_name, DATA_FILEPATH_TYPE_WORDMAP);

  FILE *stream = stream_from_filename(wmp_filename);
  if (!stream) {
    log_fatal("could not open stream for filename: %s\n", wmp_filename);
  }
  free(wmp_filename);

  wmp->name = string_duplicate(wmp_name);

  if (!read_header_from_stream(wmp, stream)) {
    log_fatal("could not read header from stream\n");
  }
}

static inline WMP *wmp_create(const char *data_paths, const char *wmp_name) {
  WMP *wmp = (WMP *)malloc_or_die(sizeof(WMP));
  wmp->name = NULL;
  wmp_load(wmp, data_paths, wmp_name);
  return wmp;
}

static inline void wmp_destroy(WMP *wmp) {
  if (!wmp) {
    return;
  }
  free(wmp->name);
  free(wmp);
}

#endif