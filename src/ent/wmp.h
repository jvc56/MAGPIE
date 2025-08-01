#ifndef WMP_H
#define WMP_H

#include "../compat/endian_conv.h"
#include "../def/board_defs.h"
#include "../def/wmp_defs.h"
#include "../ent/bit_rack.h"
#include "../util/fileproxy.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "data_filepaths.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
// WordMap binary format:
// ======================
// 1 byte: major version number
// 1 byte: board size
// 1 byte: maximum word length
// Use the following either to dynamically allocate buffers for intermediate
// and final results, or to validate that statically allocated buffers are
// large enough.
// 4 bytes: maximum size in bytes of word lookup results
// 4 bytes: maximum size in bytes of blank pair results
// xxxxxx: repeated WordOfSameLengthMap binary data

// WordOfSameLengthMap binary format:
// ==================================
// 4 bytes: number of word buckets
// num_word_buckets * 4 bytes: word bucket starts
// 4 bytes: number of word entries
// 28 * number_of_word_entries bytes: word entries
// 4 bytes: number of uninlined words
// num_unlined_words * word_length bytes: MachineLetter mls of words
// ----------------------------------
// 4 bytes: number of blank buckets
// num_blank_buckets * 4 bytes: blank bucket starts
// 4 bytes: number of blank entries
// 28 * number_of_blank_entries bytes: blank entries
// ----------------------------------
// 4 bytes: number of double blank buckets
// double_num_blank_buckets * 4 bytes: double blank bucket starts
// 4 bytes: number of double blank entries
// 28 * number_of_double_blank_entries bytes: double blank entries
// 4 bytes: number of blank letter pairs
// num_blank_letter_pairs * 2 bytes: MachineLetter mls of blank letter pairs

// WMPEntry binary format:
// ===========================
// 16 bytes: If first byte is nonzero, a complete list of contiguous anagrams,
//           terminated by a zero byte (unless it fills the whole 16 bytes).
//           If the first byte is zero,
//             If num_blanks == 0:
//                8 bytes: zeroes
//                4 bytes: start index into word_letters
//                4 bytes: number of words
//             If num_blanks == 1:
//                8 bytes: zeroes
//                4 bytes: bitvector for blank letters with solutions
//                4 bytes: zeroes
//             If num_blanks == 2:
//                8 bytes: zeroes
//                4 bytes: bitvector for first blank letters with solutions
//                4 bytes: zeroes
// 12 bytes: BitRack quotient (96 bits)
//           (number of word buckets must be high enough that maximum quotient
//           fits. largest_bit_rack_for_ld(ld) / num_word_buckets < (1 << 96)).

typedef struct __attribute__((packed)) WMPEntry {
  union {
    uint8_t bucket_or_inline[WMP_INLINE_VALUE_BYTES];
    struct {
      uint8_t nonzero_if_inlined;
      uint8_t _unused_padding1[WMP_NONINLINE_PADDING_BYTES];
      union {
        struct {
          uint32_t word_start;
          uint32_t num_words;
        };
        struct {
          uint32_t blank_letters;
          uint32_t _unused_padding2;
        };
        struct {
          uint32_t first_blank_letters;
          uint32_t _unused_padding3;
        };
      };
    };
  };
  uint8_t quotient[WMP_QUOTIENT_BYTES];
} WMPEntry;

typedef struct WMPForLength {
  // Blankless words
  uint32_t num_word_buckets;
  uint32_t num_word_entries;
  uint32_t num_uninlined_words;
  uint32_t *word_bucket_starts;
  WMPEntry *word_map_entries;
  MachineLetter *word_letters;

  // Single Blanks
  uint32_t num_blank_buckets;
  uint32_t num_blank_entries;
  uint32_t *blank_bucket_starts;
  WMPEntry *blank_map_entries;

  // Double Blanks
  uint32_t num_double_blank_buckets;
  uint32_t num_double_blank_entries;
  uint32_t *double_blank_bucket_starts;
  WMPEntry *double_blank_map_entries;
} WMPForLength;

typedef struct WMP {
  char *name;
  uint8_t version;
  uint8_t board_dim;
  uint32_t max_word_lookup_bytes;
  WMPForLength wfls[BOARD_DIM + 1];
} WMP;

static inline void read_byte_from_stream(uint8_t *byte, FILE *stream) {
  const size_t result = fread(byte, sizeof(uint8_t), 1, stream);
  if (result != 1) {
    log_fatal("could not read byte from stream");
  }
}

static inline void read_bytes_from_stream(uint8_t *bytes, size_t n,
                                          FILE *stream) {
  const size_t result = fread(bytes, sizeof(uint8_t), n, stream);
  if (result != n) {
    log_fatal("could not read bytes from stream");
  }
}

static inline void read_uint32_from_stream(uint32_t *i, FILE *stream) {
  const size_t result = fread(i, sizeof(uint32_t), 1, stream);
  if (result != 1) {
    log_fatal("could not read uint32 from stream");
  }
  *i = le32toh(*i);
}

static inline void read_uint32s_from_stream(uint32_t *i, size_t n,
                                            FILE *stream) {
  const size_t result = fread(i, sizeof(uint32_t), n, stream);
  if (result != n) {
    log_fatal("could not read uint32s from stream");
  }
}

static inline void read_wmp_entries_from_stream(WMPEntry *entries, uint32_t n,
                                                FILE *stream) {
  // FIXME(olaugh): We should not depend on the struct fields being contiguous.
  // Change this to read each field individually.
  const size_t result = fread(entries, sizeof(WMPEntry), n, stream);
  if (result != n) {
    log_fatal("could not read WMPEntries from stream");
  }
  for (uint32_t i = 0; i < n; i++) {
    for (uint32_t j = 0; j < WMP_QUOTIENT_BYTES; j++) {
      WMPEntry *entry = &entries[i];
      entry->word_start = le32toh(entry->word_start);
      entry->num_words = le32toh(entry->num_words);
    }
  }
}

static inline void read_header_from_stream(WMP *wmp, FILE *stream) {
  read_byte_from_stream(&wmp->version, stream);
  read_byte_from_stream(&wmp->board_dim, stream);
  read_uint32_from_stream(&wmp->max_word_lookup_bytes, stream);
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
  wfl->word_letters =
      (MachineLetter *)malloc_or_die(wfl->num_uninlined_words * (size_t)(len));
  read_bytes_from_stream(wfl->word_letters,
                         wfl->num_uninlined_words * (size_t)(len), stream);
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
}

static inline void read_wmp_for_length(WMP *wmp, uint32_t len, FILE *stream) {
  WMPForLength *wfl = &wmp->wfls[len];
  read_wfl_blankless_words(wfl, len, stream);
  read_wfl_blanks(wfl, stream);
  read_wfl_double_blanks(wfl, stream);
}

static inline void wmp_load_from_filename(WMP *wmp, const char *wmp_name,
                                          const char *wmp_filename,
                                          ErrorStack *error_stack) {
  FILE *stream = stream_from_filename(wmp_filename, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  read_header_from_stream(wmp, stream);
  if (wmp->version < WMP_EARLIEST_SUPPORTED_VERSION) {
    error_stack_push(
        error_stack, ERROR_STATUS_WMP_UNSUPPORTED_VERSION,
        get_formatted_string(
            "detected wmp version %d but only %d or greater is supported: %s\n",
            wmp->version, WMP_EARLIEST_SUPPORTED_VERSION, wmp_filename));
    return;
  }
  if (wmp->board_dim != BOARD_DIM) {
    error_stack_push(error_stack, ERROR_STATUS_WMP_INCOMPATIBLE_BOARD_DIM,
                     get_formatted_string(
                         "detected wmp board dimension of %d which does not "
                         "match the required board dimension of %d: %s\n",
                         wmp->board_dim, BOARD_DIM, wmp_filename));
    return;
  }

  // IMPORTANT: the name must only be set once there are no more possible
  // errors that could be encountered, otherwise, it will introduce a memory
  // leak.
  wmp->name = string_duplicate(wmp_name);

  for (uint32_t len = 2; len <= BOARD_DIM; len++) {
    read_wmp_for_length(wmp, len, stream);
  }
}

static inline void wmp_load(WMP *wmp, const char *data_paths,
                            const char *wmp_name, ErrorStack *error_stack) {
  char *wmp_filename = data_filepaths_get_readable_filename(
      data_paths, wmp_name, DATA_FILEPATH_TYPE_WORDMAP, error_stack);
  if (error_stack_is_empty(error_stack)) {
    wmp_load_from_filename(wmp, wmp_name, wmp_filename, error_stack);
  }
  free(wmp_filename);
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
  }
  free(wmp);
}

static inline WMP *wmp_create(const char *data_paths, const char *wmp_name,
                              ErrorStack *error_stack) {
  WMP *wmp = (WMP *)calloc_or_die(1, sizeof(WMP));
  wmp_load(wmp, data_paths, wmp_name, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    wmp_destroy(wmp);
    wmp = NULL;
  }
  return wmp;
}

static inline bool wmp_entry_is_inlined(const WMPEntry *entry) {
  return entry->nonzero_if_inlined != 0;
}

static inline uint32_t max_inlined_words(uint32_t word_length) {
  return WMP_INLINE_VALUE_BYTES / word_length;
}

static inline int wmp_entry_number_of_inlined_bytes(const WMPEntry *entry,
                                                    int word_length) {
  int num_bytes = (int)max_inlined_words(word_length) * word_length;
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
  memcpy(buffer, entry->bucket_or_inline, bytes_written);
  return bytes_written;
}

static inline int wmp_entry_write_uninlined_blankless_words_to_buffer(
    const WMPEntry *entry, const WMPForLength *wfl, int word_length,
    uint8_t *buffer) {
  const MachineLetter *letters = wfl->word_letters + entry->word_start;
  const int bytes_written = (int)entry->num_words * word_length;
  memcpy(buffer, letters, bytes_written);
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

static inline int
wmp_entry_write_blanks_to_buffer(const WMPEntry *entry, const WMPForLength *wfl,
                                 BitRack *bit_rack, int word_length,
                                 MachineLetter min_ml, uint8_t *buffer) {
  int bytes_written = 0;
  bit_rack_set_letter_count(bit_rack, BLANK_MACHINE_LETTER, 0);
  // NOLINTNEXTLINE(bugprone-too-small-loop-variable)
  for (MachineLetter ml = min_ml; ml < BIT_RACK_MAX_ALPHABET_SIZE; ml++) {
    if (entry->blank_letters & (1ULL << ml)) {
      bit_rack_add_letter(bit_rack, ml);
      bytes_written += wfl_write_blankless_words_to_buffer(
          wfl, bit_rack, word_length, buffer + bytes_written);
      bit_rack_take_letter(bit_rack, ml);
    }
  }
  bit_rack_set_letter_count(bit_rack, BLANK_MACHINE_LETTER, 1);
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

static inline const char *wmp_get_name(const WMP *wmp) { return wmp->name; }

static inline const WMPEntry *
wmp_get_word_entry(const WMP *wmp, const BitRack *bit_rack, int word_length) {
  const WMPForLength *wfl = &wmp->wfls[word_length];
  const WMPEntry *entry = NULL;
  switch (bit_rack_get_letter(bit_rack, BLANK_MACHINE_LETTER)) {
  case 0:
    entry = wfl_get_word_entry(wfl, bit_rack);
    break;
  case 1:
    entry = wfl_get_blank_entry(wfl, bit_rack);
    break;
  case 2:
    entry = wfl_get_double_blank_entry(wfl, bit_rack);
    break;
  default:
    break;
  }
  return entry;
}

static inline int wfl_write_blanks_to_buffer(const WMPForLength *wfl,
                                             BitRack *bit_rack, int word_length,
                                             MachineLetter min_ml,
                                             uint8_t *buffer) {
  const WMPEntry *entry = wfl_get_blank_entry(wfl, bit_rack);
  if (entry == NULL) {
    return 0;
  }
  return wmp_entry_write_blanks_to_buffer(entry, wfl, bit_rack, word_length,
                                          min_ml, buffer);
}

static inline int wmp_entry_write_double_blanks_to_buffer(
    const WMPEntry *entry, const WMPForLength *wfl, BitRack *bit_rack,
    int word_length, uint8_t *buffer) {
  int bytes_written = 0;
  bit_rack_set_letter_count(bit_rack, BLANK_MACHINE_LETTER, 1);
  // NOLINTNEXTLINE(bugprone-too-small-loop-variable)
  for (MachineLetter ml = 1; ml < BIT_RACK_MAX_ALPHABET_SIZE; ml++) {
    if (entry->blank_letters & (1ULL << ml)) {
      bit_rack_add_letter(bit_rack, ml);
      bytes_written += wfl_write_blanks_to_buffer(wfl, bit_rack, word_length,
                                                  ml, buffer + bytes_written);
      bit_rack_take_letter(bit_rack, ml);
    }
  }
  bit_rack_set_letter_count(bit_rack, BLANK_MACHINE_LETTER, 2);
  return bytes_written;
}

static inline int wmp_entry_write_words_to_buffer(const WMPEntry *entry,
                                                  const WMP *wmp,
                                                  BitRack *bit_rack,
                                                  int word_length,
                                                  uint8_t *buffer) {
  const WMPForLength *wfl = &wmp->wfls[word_length];
  int result = 0;
  switch (bit_rack_get_letter(bit_rack, BLANK_MACHINE_LETTER)) {
  case 0:
    result = wmp_entry_write_blankless_words_to_buffer(entry, wfl, word_length,
                                                       buffer);
    break;
  case 1:
    result = wmp_entry_write_blanks_to_buffer(entry, wfl, bit_rack, word_length,
                                              1, buffer);
    break;
  case 2:
    result = wmp_entry_write_double_blanks_to_buffer(entry, wfl, bit_rack,
                                                     word_length, buffer);
    break;
  default:
    break;
  }
  return result;
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

static inline void write_byte_to_stream_or_die(uint8_t byte, FILE *stream,
                                               const char *description) {
  fwrite_or_die(&byte, sizeof(byte), 1, stream, description);
}

static inline void write_uint32_to_stream_or_die(uint32_t i, FILE *stream,
                                                 const char *description) {
  fwrite_or_die(&i, sizeof(i), 1, stream, description);
}

static inline void write_wfl_to_stream(int length, const WMPForLength *wfl,
                                       FILE *stream) {
  write_uint32_to_stream_or_die(wfl->num_word_buckets, stream,
                                "num word buckets");
  fwrite_or_die(wfl->word_bucket_starts, sizeof(uint32_t),
                wfl->num_word_buckets + 1, stream, "word bucket starts");
  write_uint32_to_stream_or_die(wfl->num_word_entries, stream,
                                "num word entries");
  fwrite_or_die(wfl->word_map_entries, sizeof(WMPEntry), wfl->num_word_entries,
                stream, "word map entries");
  write_uint32_to_stream_or_die(wfl->num_uninlined_words, stream,
                                "num uninlined words");
  fwrite_or_die(wfl->word_letters, sizeof(MachineLetter),
                wfl->num_uninlined_words * (size_t)length, stream,
                "word letters");
  write_uint32_to_stream_or_die(wfl->num_blank_buckets, stream,
                                "num blank buckets");
  fwrite_or_die(wfl->blank_bucket_starts, sizeof(uint32_t),
                wfl->num_blank_buckets + 1, stream, "blank bucket starts");
  write_uint32_to_stream_or_die(wfl->num_blank_entries, stream,
                                "num blank entries");
  fwrite_or_die(wfl->blank_map_entries, sizeof(WMPEntry),
                wfl->num_blank_entries, stream, "blank map entries");
  write_uint32_to_stream_or_die(wfl->num_double_blank_buckets, stream,
                                "num double blank buckets");
  fwrite_or_die(wfl->double_blank_bucket_starts, sizeof(uint32_t),
                wfl->num_double_blank_buckets + 1, stream,
                "double blank bucket starts");
  write_uint32_to_stream_or_die(wfl->num_double_blank_entries, stream,
                                "num double blank entries");
  fwrite_or_die(wfl->double_blank_map_entries, sizeof(WMPEntry),
                wfl->num_double_blank_entries, stream,
                "double blank map entries");
}

static inline void wmp_write_to_file(const WMP *wmp, const char *filename,
                                     ErrorStack *error_stack) {
  FILE *stream = fopen_safe(filename, "wb", error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  write_byte_to_stream_or_die(wmp->version, stream, "wmp version");
  write_byte_to_stream_or_die(wmp->board_dim, stream, "wmp board dim");
  write_uint32_to_stream_or_die(wmp->max_word_lookup_bytes, stream,
                                "wmp max word lookup bytes");
  for (int len = 2; len <= BOARD_DIM; len++) {
    write_wfl_to_stream(len, &wmp->wfls[len], stream);
  }
  fclose_or_die(stream);
}

#endif