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

#include "../ent/bit_rack.h"

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

int wfl_write_blankless_words_to_buffer(const WMPForLength *wfl,
                                        const BitRack *bit_rack,
                                        int word_length, uint8_t *buffer) {
  BitRack quotient;
  uint32_t bucket_index;
  bit_rack_div_mod(bit_rack, wfl->num_word_buckets, &quotient, &bucket_index);
  const uint32_t start = wfl->word_bucket_starts[bucket_index];
  const uint32_t end = wfl->word_bucket_starts[bucket_index + 1];
  for (uint32_t i = start; i < end; i++) {
    const WMPEntry *entry = &wfl->word_map_entries[i];
    const BitRack entry_quotient = bit_rack_read_12_bytes(entry->quotient);
    if (!bit_rack_equals(&entry_quotient, &quotient)) {
      continue;
    }
    const uint64_t expected_zero = *((uint64_t *)entry->bucket_or_inline);
    assert(expected_zero == 0);
    const uint32_t word_start = *((uint32_t *)entry->bucket_or_inline + 2);
    const uint32_t num_words = *((uint32_t *)entry->bucket_or_inline + 3);
    const uint8_t *letters = wfl->word_letters + word_start;
    const int bytes_written = num_words * word_length;
    memory_copy(buffer, letters, bytes_written);
    return bytes_written;
  }
  return 0;
}

int wfl_write_blanks_to_buffer(const WMPForLength *wfl, BitRack *bit_rack,
                               int word_length, uint8_t *buffer) {
  BitRack quotient;
  uint32_t bucket_index;
  bit_rack_div_mod(bit_rack, wfl->num_blank_buckets, &quotient, &bucket_index);
  const uint32_t start = wfl->blank_bucket_starts[bucket_index];
  const uint32_t end = wfl->blank_bucket_starts[bucket_index + 1];
  int bytes_written = 0;
  for (uint32_t i = start; i < end; i++) {
    const WMPEntry *entry = &wfl->blank_map_entries[i];
    const BitRack entry_quotient = bit_rack_read_12_bytes(entry->quotient);
    if (!bit_rack_equals(&entry_quotient, &quotient)) {
      continue;
    }
    const uint64_t expected_zero = *((uint64_t *)entry->bucket_or_inline);
    assert(expected_zero == 0);
    const uint32_t blank_letters = *((uint32_t *)entry->bucket_or_inline + 2);
    bit_rack_set_letter_count(bit_rack, BLANK_MACHINE_LETTER, 0);
    for (uint8_t ml = 1; ml < BIT_RACK_MAX_ALPHABET_SIZE; ml++) {
      if (blank_letters & (1ULL << ml)) {
        bit_rack_add_letter(bit_rack, ml);
        bytes_written += wfl_write_blankless_words_to_buffer(
            wfl, bit_rack, word_length, buffer + bytes_written);
        bit_rack_take_letter(bit_rack, ml);
      }
    }
    bit_rack_set_letter_count(bit_rack, BLANK_MACHINE_LETTER, 1);
    return bytes_written;
  }
  return bytes_written;
}

int wfl_write_double_blanks_to_buffer(const WMPForLength *wfl,
                                      BitRack *bit_rack, int word_length,
                                      uint8_t *buffer) {
  BitRack quotient;
  uint32_t bucket_index;
  bit_rack_div_mod(bit_rack, wfl->num_double_blank_buckets, &quotient,
                   &bucket_index);
  int bytes_written = 0;
  const uint32_t start = wfl->double_blank_bucket_starts[bucket_index];
  const uint32_t end = wfl->double_blank_bucket_starts[bucket_index + 1];
  for (uint32_t i = start; i < end; i++) {
    const WMPEntry *entry = &wfl->double_blank_map_entries[i];
    const BitRack entry_quotient = bit_rack_read_12_bytes(entry->quotient);
    if (!bit_rack_equals(&entry_quotient, &quotient)) {
      continue;
    }
    const uint64_t expected_zero = *((uint64_t *)entry->bucket_or_inline);
    assert(expected_zero == 0);
    const uint32_t pair_start = *((uint32_t *)entry->bucket_or_inline + 2);
    const uint32_t num_pairs = *((uint32_t *)entry->bucket_or_inline + 3);
    const uint8_t *pairs = wfl->double_blank_letters + pair_start;
    bit_rack_set_letter_count(bit_rack, BLANK_MACHINE_LETTER, 0);
    for (uint32_t j = 0; j < num_pairs; j++) {
      const uint8_t ml1 = pairs[j * 2];
      const uint8_t ml2 = pairs[j * 2 + 1];
      bit_rack_add_letter(bit_rack, ml1);
      bit_rack_add_letter(bit_rack, ml2);
      bytes_written += wfl_write_blankless_words_to_buffer(
          wfl, bit_rack, word_length, buffer + bytes_written);
      bit_rack_take_letter(bit_rack, ml2);
      bit_rack_take_letter(bit_rack, ml1);
    }
    bit_rack_set_letter_count(bit_rack, BLANK_MACHINE_LETTER, 2);
    return bytes_written;
  }
  return bytes_written;
}

int wmp_write_words_to_buffer(const WMP *wmp, BitRack *bit_rack,
                              int word_length, uint8_t *buffer) {
  const WMPForLength *wfl = &wmp->maps[word_length];
  switch (bit_rack_get_letter(bit_rack, BLANK_MACHINE_LETTER)) {
  case 0:
    return wfl_write_blankless_words_to_buffer(wfl, bit_rack, word_length,
                                               buffer);
  case 1:
    return wfl_write_blanks_to_buffer(wfl, bit_rack, word_length, buffer);
  case 2:
    return wfl_write_double_blanks_to_buffer(wfl, bit_rack, word_length,
                                             buffer);
  default:
    return 0;
  }
}

#endif