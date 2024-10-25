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
  // Sizes
  uint32_t num_word_buckets;
  uint32_t num_blank_buckets;
  uint32_t num_double_blank_buckets;
  uint32_t num_word_entries;
  uint32_t num_blank_entries;
  uint32_t num_double_blank_entries;
  uint32_t num_words;
  uint32_t num_blank_pairs;

  // Hash bucket starts
  uint32_t *word_bucket_starts;
  uint32_t *blank_bucket_starts;
  uint32_t *double_blank_bucket_starts;

  // Hash entries
  WMPEntry *word_map_entries;
  WMPEntry *blank_map_entries;
  WMPEntry *double_blank_map_entries;

  // Letter blobs
  uint8_t *word_letters;
  uint8_t *double_blank_letters;
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

static inline void read_byte_from_stream(uint8_t *byte, FILE *stream) {
  const size_t result = fread(byte, sizeof(uint8_t), 1, stream);
  if (result != 1) {
    log_fatal("could not read byte from stream\n");
  }
}

static inline void read_bytes_from_stream(uint8_t *bytes, size_t n,
                                          FILE *stream) {
  const size_t result = fread(bytes, sizeof(uint8_t), n, stream);
  if (result != n) {
    log_fatal("could not read bytes from stream\n");
  }
}

static inline void read_uint32_from_stream(uint32_t *i, FILE *stream) {
  const size_t result = fread(i, sizeof(uint32_t), 1, stream);
  if (result != 1) {
    log_fatal("could not read uint32 from stream\n");
  }
}

static inline void read_uint32s_from_stream(uint32_t *i, size_t n,
                                            FILE *stream) {
  const size_t result = fread(i, sizeof(uint32_t), n, stream);
  if (result != n) {
    log_fatal("could not read uint32s from stream\n");
  }
}

static inline void read_wmp_entries_from_stream(WMPEntry *entries, uint32_t n,
                                                FILE *stream) {
  // FIXME(olaugh): We should not depend on the struct fields being contiguous.
  // Change this to read each field individually.
  const size_t result = fread(entries, sizeof(WMPEntry), n, stream);
  if (result != n) {
    log_fatal("could not read WMPEntries from stream\n");
  }
}

static inline void read_header_from_stream(WMP *wmp, FILE *stream) {
  read_byte_from_stream(&wmp->major_version, stream);
  read_byte_from_stream(&wmp->minor_version, stream);
  read_byte_from_stream(&wmp->min_word_length, stream);
  read_byte_from_stream(&wmp->max_word_length, stream);
  read_uint32_from_stream(&wmp->max_word_lookup_bytes, stream);
  read_uint32_from_stream(&wmp->max_blank_pair_bytes, stream);
}

static inline void read_wfl_blankless_words(WMPForLength *wfl, uint32_t len,
                                  FILE *stream) {
  read_uint32_from_stream(&wfl->num_word_buckets, stream);
  wfl->word_bucket_starts =
      (uint32_t *)malloc_or_die((wfl->num_word_buckets + 1) * sizeof(uint32_t));
  read_uint32s_from_stream(wfl->word_bucket_starts, wfl->num_word_buckets + 1,
                           stream);

  read_uint32_from_stream(&wfl->num_word_entries, stream);
  wfl->word_map_entries =
      (WMPEntry *)malloc_or_die(wfl->num_word_entries * sizeof(WMPEntry));
  read_wmp_entries_from_stream(wfl->word_map_entries, wfl->num_word_entries,
                               stream);

  read_uint32_from_stream(&wfl->num_words, stream);
  wfl->word_letters = (uint8_t *)malloc_or_die(wfl->num_words * len);
  read_bytes_from_stream(wfl->word_letters, wfl->num_words * len, stream);
}

static inline void read_wfl_blanks(WMPForLength *wfl, FILE *stream) {
  read_uint32_from_stream(&wfl->num_blank_buckets, stream);
  wfl->blank_bucket_starts = (uint32_t *)malloc_or_die(
      (wfl->num_blank_buckets + 1) * sizeof(uint32_t));
  read_uint32s_from_stream(wfl->blank_bucket_starts, wfl->num_blank_buckets + 1,
                           stream);

  read_uint32_from_stream(&wfl->num_blank_entries, stream);
  wfl->blank_map_entries =
      (WMPEntry *)malloc_or_die(wfl->num_blank_entries * sizeof(WMPEntry));
  read_wmp_entries_from_stream(wfl->blank_map_entries, wfl->num_blank_entries,
                               stream);                           
}

static inline void read_wfl_double_blanks(WMPForLength *wfl, FILE *stream) {
  read_uint32_from_stream(&wfl->num_double_blank_buckets, stream);
  wfl->double_blank_bucket_starts = (uint32_t *)malloc_or_die(
      (wfl->num_double_blank_buckets + 1) * sizeof(uint32_t));
  read_uint32s_from_stream(wfl->double_blank_bucket_starts,
                           wfl->num_double_blank_buckets + 1, stream);

  read_uint32_from_stream(&wfl->num_double_blank_entries, stream);
  wfl->double_blank_map_entries =
      (WMPEntry *)malloc_or_die(wfl->num_double_blank_entries * sizeof(WMPEntry));
  read_wmp_entries_from_stream(wfl->double_blank_map_entries,
                               wfl->num_double_blank_entries, stream);

  read_uint32_from_stream(&wfl->num_blank_pairs, stream);
  wfl->double_blank_letters = (uint8_t *)malloc_or_die(
      wfl->num_blank_pairs * 2 * sizeof(uint8_t));
  read_bytes_from_stream(wfl->double_blank_letters, wfl->num_blank_pairs * 2,
                         stream);                               
}

static inline void read_wmp_for_length(WMP *wmp, uint32_t len, FILE *stream) {
  WMPForLength *wfl = &wmp->maps[len];
  read_wfl_blankless_words(wfl, len, stream);
  read_wfl_blanks(wfl, stream);
  read_wfl_double_blanks(wfl, stream);
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

  read_header_from_stream(wmp, stream);
  if (wmp->min_word_length > wmp->max_word_length) {
    log_fatal("min_word_length > max_word_length\n");
  }
  if (wmp->max_word_length > BOARD_DIM) {
    log_fatal("max_word_length > BOARD_DIM\n");
  }

  for (uint32_t len = wmp->min_word_length; len <= wmp->max_word_length;
       len++) {
    read_wmp_for_length(wmp, len, stream);
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