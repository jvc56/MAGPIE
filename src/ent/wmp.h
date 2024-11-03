#ifndef WMP_H
#define WMP_H

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
  // Blankless words
  uint32_t num_word_buckets;
  uint32_t num_word_entries;
  uint32_t num_uninlined_words;
  uint32_t *word_bucket_starts;
  WMPEntry *word_map_entries;
  uint8_t *word_letters;

  // Single Blanks
  uint32_t num_blank_buckets;
  uint32_t num_blank_entries;
  uint32_t *blank_bucket_starts;
  WMPEntry *blank_map_entries;

  // Double Blanks
  uint32_t num_double_blank_buckets;
  uint32_t num_double_blank_entries;
  uint32_t num_blank_pairs;
  uint32_t *double_blank_bucket_starts;
  WMPEntry *double_blank_map_entries;
  uint8_t *double_blank_letters;
} WMPForLength;

typedef struct WMP {
  char *name;
  uint8_t version;
  uint8_t board_dim;
  uint32_t max_blank_pair_bytes;
  uint32_t max_word_lookup_bytes;
  WMPForLength wfls[BOARD_DIM + 1];
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
  read_byte_from_stream(&wmp->version, stream);
  read_byte_from_stream(&wmp->board_dim, stream);
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

  read_uint32_from_stream(&wfl->num_uninlined_words, stream);
  wfl->word_letters = (uint8_t *)malloc_or_die(wfl->num_uninlined_words * len);
  read_bytes_from_stream(wfl->word_letters, wfl->num_uninlined_words * len,
                         stream);
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
  wfl->double_blank_map_entries = (WMPEntry *)malloc_or_die(
      wfl->num_double_blank_entries * sizeof(WMPEntry));
  read_wmp_entries_from_stream(wfl->double_blank_map_entries,
                               wfl->num_double_blank_entries, stream);

  read_uint32_from_stream(&wfl->num_blank_pairs, stream);
  wfl->double_blank_letters =
      (uint8_t *)malloc_or_die(wfl->num_blank_pairs * 2 * sizeof(uint8_t));
  read_bytes_from_stream(wfl->double_blank_letters, wfl->num_blank_pairs * 2,
                         stream);
}

static inline void read_wmp_for_length(WMP *wmp, uint32_t len, FILE *stream) {
  WMPForLength *wfl = &wmp->wfls[len];
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
  if (wmp->version < WORD_MAP_EARLIEST_SUPPORTED_VERSION) {
    log_fatal("wmp->version < WORD_MAP_EARLIEST_SUPPORTED_VERSION\n");
  }
  if (wmp->board_dim != BOARD_DIM) {
    log_fatal("wmp->board_dim != BOARD_DIM\n");
  }

  for (uint32_t len = 2; len <= BOARD_DIM; len++) {
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
  if (wmp == NULL) {
    return;
  }
  if (wmp->name != NULL) {
    free(wmp->name);
  }
  // wmp->wfls[0] and wmp->wfls[1] should have uninitialized data
  // and no allocated arrays.
  for (int len = 2; len <= wmp->board_dim; len++) {
    WMPForLength *wfl = &wmp->wfls[len];
    free(wfl->word_bucket_starts);
    free(wfl->word_map_entries);
    free(wfl->word_letters);

    free(wfl->blank_bucket_starts);
    free(wfl->blank_map_entries);

    free(wfl->double_blank_bucket_starts);
    free(wfl->double_blank_map_entries);
    free(wfl->double_blank_letters);
  }
  free(wmp);
}

static inline bool wmp_entry_is_inlined(const WMPEntry *entry) {
  return entry->bucket_or_inline[0] != 0;
}

static inline uint32_t max_inlined_words(int word_length) {
  return WMP_INLINE_VALUE_BYTES / word_length;
}

static inline int wmp_entry_number_of_inlined_bytes(const WMPEntry *entry,
                                                    int word_length) {
  int num_bytes = max_inlined_words(word_length) * word_length;
  while (num_bytes > word_length) {
    const int byte_idx = num_bytes - 1;
    if (entry->bucket_or_inline[byte_idx] != 0) {
      break;
    }
    num_bytes -= word_length;
  }
  return num_bytes;
}

static inline int wmp_entry_write_inlined_blankless_words_to_buffer(
    const WMPEntry *entry, int word_length, uint8_t *buffer) {
  const int bytes_written =
      wmp_entry_number_of_inlined_bytes(entry, word_length);
  memory_copy(buffer, entry->bucket_or_inline, bytes_written);
  return bytes_written;
}

// Used for both blankless words and double blanks.
// Single blank entries also store a uint32_t in this position but there's
// a separate equivalent function since I feel that's semantically different.
static inline uint32_t wmp_entry_get_word_or_pair_start(const WMPEntry *entry) {
  uint32_t word_or_pair_start;
  // 8 bytes empty at the start of the entry
  // I think this working depends on WMPEntry and these embedded uint32_ts being
  // 4-byte word aligned.
  memory_copy(
      &word_or_pair_start,
      (uint32_t *)(entry->bucket_or_inline + WORD_MAP_WORD_START_OFFSET_BYTES),
      sizeof(word_or_pair_start));
  return word_or_pair_start;
}

// Used for both blankless words and double blanks.
static inline uint32_t wmp_entry_get_num_words_or_pairs(const WMPEntry *entry) {
  uint32_t num_words_or_pairs;
  // num_words follows 8 bytes of empty space and 4 bytes for word_start
  // I think this working depends on WMPEntry and these embedded uint32_ts being
  // 4-byte word aligned.
  memory_copy(
      &num_words_or_pairs,
      (uint32_t *)(entry->bucket_or_inline + WORD_MAP_NUM_WORDS_OFFSET_BYTES),
      sizeof(num_words_or_pairs));
  return num_words_or_pairs;
}

static inline int wmp_entry_write_uninlined_blankless_words_to_buffer(
    const WMPEntry *entry, const WMPForLength *wfl, int word_length,
    uint8_t *buffer) {
  const uint32_t word_start = wmp_entry_get_word_or_pair_start(entry);
  const uint32_t num_words = wmp_entry_get_num_words_or_pairs(entry);
  const uint8_t *letters = wfl->word_letters + word_start;
  const int bytes_written = num_words * word_length;
  memory_copy(buffer, letters, bytes_written);
  return bytes_written;
}

static inline int
wmp_entry_write_blankless_words_to_buffer(const WMPEntry *entry,
                                          const WMPForLength *wfl,
                                          int word_length, uint8_t *buffer) {
  if (wmp_entry_is_inlined(entry)) {
    return wmp_entry_write_inlined_blankless_words_to_buffer(entry, word_length,
                                                             buffer);
  }
  return wmp_entry_write_uninlined_blankless_words_to_buffer(
      entry, wfl, word_length, buffer);
}

static inline const WMPEntry *wfl_get_word_entry(const WMPForLength *wfl,
                                                 const BitRack *bit_rack) {
  BitRack quotient;
  uint32_t bucket_index;
  bit_rack_div_mod(bit_rack, wfl->num_word_buckets, &quotient, &bucket_index);
  const uint32_t start = wfl->word_bucket_starts[bucket_index];
  const uint32_t end = wfl->word_bucket_starts[bucket_index + 1];
  for (uint32_t i = start; i < end; i++) {
    const WMPEntry *entry = &wfl->word_map_entries[i];
    const BitRack entry_quotient = bit_rack_read_12_bytes(entry->quotient);
    if (bit_rack_equals(&entry_quotient, &quotient)) {
      return entry;
    }
  }
  return NULL;
}

static inline int wfl_write_blankless_words_to_buffer(const WMPForLength *wfl,
                                                      const BitRack *bit_rack,
                                                      int word_length,
                                                      uint8_t *buffer) {
  const WMPEntry *entry = wfl_get_word_entry(wfl, bit_rack);
  if (entry == NULL) {
    return 0;
  }
  return wmp_entry_write_blankless_words_to_buffer(entry, wfl, word_length,
                                                   buffer);
}

static inline uint32_t wmp_entry_get_blank_letters(const WMPEntry *entry) {
  uint32_t blank_letters;
  // 32-bit bitvector follows 8 bytes of empty space
  memory_copy(&blank_letters,
              (uint32_t *)(entry->bucket_or_inline +
                           WORD_MAP_BLANK_LETTERS_OFFSET_BYTES),
              sizeof(blank_letters));
  return blank_letters;
}

static inline int wmp_entry_write_blanks_to_buffer(const WMPEntry *entry,
                                                   const WMPForLength *wfl,
                                                   BitRack *bit_rack,
                                                   int word_length,
                                                   uint8_t *buffer) {
  const uint32_t blank_letters = wmp_entry_get_blank_letters(entry);
  int bytes_written = 0;
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

static inline int wfl_write_blanks_to_buffer(const WMPForLength *wfl,
                                             BitRack *bit_rack, int word_length,
                                             uint8_t *buffer) {
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
    uint32_t blank_letters = wmp_entry_get_blank_letters(entry);
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

static inline int wmp_entry_write_double_blanks_to_buffer(
    const WMPEntry *entry, const WMPForLength *wfl, BitRack *bit_rack,
    int word_length, uint8_t *buffer) {
  const uint32_t pair_start = wmp_entry_get_word_or_pair_start(entry);
  const uint32_t num_pairs = wmp_entry_get_num_words_or_pairs(entry);
  const uint8_t *pairs = wfl->double_blank_letters + pair_start;
  int bytes_written = 0;
  for (uint32_t i = 0; i < num_pairs; i++) {
    const uint8_t ml1 = pairs[i * 2];
    const uint8_t ml2 = pairs[i * 2 + 1];
    bit_rack_set_letter_count(bit_rack, BLANK_MACHINE_LETTER, 0);
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

static inline int wfl_write_double_blanks_to_buffer(const WMPForLength *wfl,
                                                    BitRack *bit_rack,
                                                    int word_length,
                                                    uint8_t *buffer) {
  if (wfl->num_double_blank_buckets == 0) {
    return 0;
  }
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
    const uint32_t pair_start = wmp_entry_get_word_or_pair_start(entry);
    const uint32_t num_pairs = wmp_entry_get_num_words_or_pairs(entry);
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

static inline const WMPEntry *wfl_get_blank_entry(const WMPForLength *wfl,
                                                  const BitRack *bit_rack) {
  if (wfl->num_blank_buckets == 0) {
    return NULL;
  }
  BitRack quotient;
  uint32_t bucket_index;
  bit_rack_div_mod(bit_rack, wfl->num_blank_buckets, &quotient, &bucket_index);
  const uint32_t start = wfl->blank_bucket_starts[bucket_index];
  const uint32_t end = wfl->blank_bucket_starts[bucket_index + 1];
  for (uint32_t i = start; i < end; i++) {
    const WMPEntry *entry = &wfl->blank_map_entries[i];
    const BitRack entry_quotient = bit_rack_read_12_bytes(entry->quotient);
    if (bit_rack_equals(&entry_quotient, &quotient)) {
      return entry;
    }
  }
  return NULL;
}

static inline const WMPEntry *
wfl_get_double_blank_entry(const WMPForLength *wfl, const BitRack *bit_rack) {
  if (wfl->num_double_blank_buckets == 0) {
    return NULL;
  }
  BitRack quotient;
  uint32_t bucket_index;
  bit_rack_div_mod(bit_rack, wfl->num_double_blank_buckets, &quotient,
                   &bucket_index);
  const uint32_t start = wfl->double_blank_bucket_starts[bucket_index];
  const uint32_t end = wfl->double_blank_bucket_starts[bucket_index + 1];
  for (uint32_t i = start; i < end; i++) {
    const WMPEntry *entry = &wfl->double_blank_map_entries[i];
    const BitRack entry_quotient = bit_rack_read_12_bytes(entry->quotient);
    if (bit_rack_equals(&entry_quotient, &quotient)) {
      return entry;
    }
  }
  return NULL;
}

static inline const WMPEntry *
wmp_get_word_entry(const WMP *wmp, const BitRack *bit_rack, int word_length) {
  const WMPForLength *wfl = &wmp->wfls[word_length];
  switch (bit_rack_get_letter(bit_rack, BLANK_MACHINE_LETTER)) {
  case 0:
    return wfl_get_word_entry(wfl, bit_rack);
  case 1:
    return wfl_get_blank_entry(wfl, bit_rack);
  case 2:
    return wfl_get_double_blank_entry(wfl, bit_rack);
  }
  return NULL;
}

static inline int wmp_entry_write_words_to_buffer(const WMPEntry *entry,
                                                  const WMP *wmp,
                                                  BitRack *bit_rack,
                                                  int word_length,
                                                  uint8_t *buffer) {
  const WMPForLength *wfl = &wmp->wfls[word_length];
  switch (bit_rack_get_letter(bit_rack, BLANK_MACHINE_LETTER)) {
  case 0:
    return wmp_entry_write_blankless_words_to_buffer(entry, wfl, word_length,
                                                     buffer);
  case 1:
    return wmp_entry_write_blanks_to_buffer(entry, wfl, bit_rack, word_length,
                                            buffer);
  case 2:
    return wmp_entry_write_double_blanks_to_buffer(entry, wfl, bit_rack,
                                                   word_length, buffer);
  }
  return 0;
}

static inline int wmp_write_words_to_buffer(const WMP *wmp, BitRack *bit_rack,
                                            int word_length, uint8_t *buffer) {
  const WMPEntry *entry = wmp_get_word_entry(wmp, bit_rack, word_length);
  if (entry == NULL) {
    return 0;
  }
  return wmp_entry_write_words_to_buffer(entry, wmp, bit_rack, word_length,
                                         buffer);
}

static inline bool wmp_has_word(const WMP *wmp, const BitRack *bit_rack,
                                int word_length) {
  return wmp_get_word_entry(wmp, bit_rack, word_length) != NULL;
}

static inline bool write_byte_to_stream(uint8_t byte, FILE *stream) {
  const size_t result = fwrite(&byte, sizeof(byte), 1, stream);
  return result == 1;
}

static inline bool write_uint32_to_stream(uint32_t i, FILE *stream) {
  const size_t result = fwrite(&i, sizeof(i), 1, stream);
  return result == 1;
}

static inline bool write_wfl_to_stream(int length, const WMPForLength *wfl,
                                       FILE *stream) {
  if (!write_uint32_to_stream(wfl->num_word_buckets, stream)) {
    printf("num word buckets: %d\n", wfl->num_word_buckets);
    return false;
  }
  size_t result = fwrite(wfl->word_bucket_starts, sizeof(uint32_t),
                         wfl->num_word_buckets + 1, stream);
  if (result != wfl->num_word_buckets + 1) {
    printf("word bucket starts: %d\n", wfl->num_word_buckets);
    return false;
  }
  if (!write_uint32_to_stream(wfl->num_word_entries, stream)) {
    printf("num word entries: %d\n", wfl->num_word_entries);
    return false;
  }
  result = fwrite(wfl->word_map_entries, sizeof(WMPEntry),
                  wfl->num_word_entries, stream);
  if (result != wfl->num_word_entries) {
    printf("word map entries: %d\n", wfl->num_word_entries);
    return false;
  }
  if (!write_uint32_to_stream(wfl->num_uninlined_words, stream)) {
    printf("num uninlined words: %d\n", wfl->num_uninlined_words);
    return false;
  }
  result = fwrite(wfl->word_letters, sizeof(uint8_t),
                  wfl->num_uninlined_words * length, stream);
  if (result != wfl->num_uninlined_words * length) {
    printf("word letters: %d\n", wfl->num_uninlined_words * length);
    return false;
  }
  if (!write_uint32_to_stream(wfl->num_blank_buckets, stream)) {
    printf("num blank buckets");
    return false;
  }
  result = fwrite(wfl->blank_bucket_starts, sizeof(uint32_t),
                  wfl->num_blank_buckets + 1, stream);
  if (result != wfl->num_blank_buckets + 1) {
    printf("blank buckets starts: %d\n", wfl->num_blank_buckets);
    return false;
  }
  if (!write_uint32_to_stream(wfl->num_blank_entries, stream)) {
    printf("num blank entries: %d\n", wfl->num_blank_entries);
    return false;
  }
  result = fwrite(wfl->blank_map_entries, sizeof(WMPEntry),
                  wfl->num_blank_entries, stream);
  if (result != wfl->num_blank_entries) {
    printf("num blank entries: %d\n", wfl->num_blank_entries);
    return false;
  }
  if (!write_uint32_to_stream(wfl->num_double_blank_buckets, stream)) {
    printf("num double blank buckets: %d\n", wfl->num_double_blank_buckets);
    return false;
  }
  result = fwrite(wfl->double_blank_bucket_starts, sizeof(uint32_t),
                  wfl->num_double_blank_buckets + 1, stream);
  if (result != wfl->num_double_blank_buckets + 1) {
    printf("double blank bucket starts: %d\n", wfl->num_double_blank_buckets);
    return false;
  }
  if (!write_uint32_to_stream(wfl->num_double_blank_entries, stream)) {
    printf("num double blank entries: %d\n", wfl->num_double_blank_entries);
    return false;
  }
  result = fwrite(wfl->double_blank_map_entries, sizeof(WMPEntry),
                  wfl->num_double_blank_entries, stream);
  if (result != wfl->num_double_blank_entries) {
    printf("double blank entries: %d\n", wfl->num_double_blank_entries);
    return false;
  }
  if (!write_uint32_to_stream(wfl->num_blank_pairs, stream)) {
    printf("num blank pairs: %d\n", wfl->num_blank_pairs);
    return false;
  }
  result = fwrite(wfl->double_blank_letters, sizeof(uint8_t),
                  wfl->num_blank_pairs * 2, stream);
  if (result != wfl->num_blank_pairs * 2) {
    printf("blank pairs: %d\n", wfl->num_blank_pairs);
    return false;
  }
  return true;
}

static inline bool wmp_write_to_file(const WMP *wmp, const char *filename) {
  FILE *stream = fopen(filename, "wb");
  if (!stream) {
    log_error("could not open file for writing: %s\n", filename);
    return false;
  }
  if (!write_byte_to_stream(wmp->version, stream)) {
    log_error("could not write version to stream\n");
    return false;
  }
  if (!write_byte_to_stream(wmp->board_dim, stream)) {
    printf("could not write board_dim to stream\n");
    return false;
  }
  if (!write_uint32_to_stream(wmp->max_word_lookup_bytes, stream)) {
    printf("could not write max word lookup bytes to stream\n");
    return false;
  }
  if (!write_uint32_to_stream(wmp->max_blank_pair_bytes, stream)) {
    printf("could not write max blank pair bytes to stream\n");
    return false;
  }
  for (int len = 2; len <= BOARD_DIM; len++) {
    if (!write_wfl_to_stream(len, &wmp->wfls[len], stream)) {
      printf("could not write words of same length map to stream\n");
      return false;
    }
  }
  if (fclose(stream) != 0) {
    printf("could not close WMP file after writing\n");
    return false;
  }
  return true;
}
#endif