#ifndef WMP_H
#define WMP_H

#include "../compat/endian_conv.h"
#include "../compat/malloc.h"
#include "../def/board_defs.h"
#include "../def/wmp_defs.h"
#include "../ent/bit_rack.h"
#include "../util/fileproxy.h"
#include "../util/fnv.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "data_filepaths.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
// 16 bytes: Full BitRack (128 bits) for collision detection
//           (stored in little-endian byte order)

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
  uint8_t bit_rack_bytes[WMP_BITRACK_BYTES];
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

  // Blankless lookup index, built from the word entries when the WMP is
  // loaded or made. It holds copies of the entries in buckets of
  // WMP_INDEX_BUCKET_SLOTS, chosen by the top bits of wmp_index_hash; a full
  // bucket spills into the next one, and an empty slot (an all-zero key) ends
  // a probe. A lookup reads one bucket instead of a bucket start and then an
  // entry.
  WMPEntry *word_index_slots;
  uint32_t word_index_bucket_mask;
  uint32_t word_index_shift;
  // Blocked Bloom filter over the same hash: each entry sets two bits of one
  // 64-bit word. Most absent racks fail it without reading the index.
  uint64_t *word_filter;
  uint32_t word_filter_shift;
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
  for (size_t idx = 0; idx < n; idx++) {
    i[idx] = le32toh(i[idx]);
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
    WMPEntry *entry = &entries[i];
    entry->word_start = le32toh(entry->word_start);
    entry->num_words = le32toh(entry->num_words);
    // BitRack bytes are already in little-endian format from file
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

// Read full BitRack from WMPEntry
static inline BitRack wmp_entry_read_bit_rack(const WMPEntry *entry) {
#if IS_LITTLE_ENDIAN
  BitRack bit_rack;
  memcpy(&bit_rack, entry->bit_rack_bytes, WMP_BITRACK_BYTES);
  return bit_rack;
#else
  // Handle big-endian case
  uint64_t low, high;
  memcpy(&low, entry->bit_rack_bytes, 8);
  memcpy(&high, entry->bit_rack_bytes + 8, 8);
  low = le64toh(low);
  high = le64toh(high);
  return (BitRack){.low = low, .high = high};
#endif
}

// Linear in the BitRack's two 64-bit halves, so it costs two independent
// multiplies. Its top bits depend on every input bit: they choose the index
// bucket and the filter word, and the bits just below them the filter bits.
static inline uint64_t wmp_index_hash(const BitRack *bit_rack) {
  return bit_rack_get_low_64(bit_rack) * WMP_INDEX_HASH_LOW_MULTIPLIER +
         bit_rack_get_high_64(bit_rack) * WMP_INDEX_HASH_HIGH_MULTIPLIER;
}

static inline uint32_t wmp_index_shift_for_power_of_2(uint32_t power_of_2) {
  uint32_t log2 = 0;
  while ((1U << log2) < power_of_2) {
    log2++;
  }
  return 64 - log2;
}

// The two bits a rack with this hash sets in its filter word.
static inline uint64_t wmp_filter_word_bits(uint64_t hash,
                                            uint32_t filter_shift) {
  const uint64_t below_word_index = hash << (64 - filter_shift);
  const uint64_t first_bit =
      below_word_index >> (64 - WMP_FILTER_BIT_INDEX_BITS);
  const uint64_t second_bit = (below_word_index << WMP_FILTER_BIT_INDEX_BITS) >>
                              (64 - WMP_FILTER_BIT_INDEX_BITS);
  return (1ULL << first_bit) | (1ULL << second_bit);
}

static inline bool wmp_entry_key_is_empty(const WMPEntry *entry) {
  const BitRack key = wmp_entry_read_bit_rack(entry);
  return (bit_rack_get_low_64(&key) | bit_rack_get_high_64(&key)) == 0;
}

static inline void wfl_build_word_index(WMPForLength *wfl) {
  const uint32_t num_entries = wfl->num_word_entries;
  const uint64_t min_slots =
      (((uint64_t)num_entries * 100) + WMP_INDEX_MAX_FILL_PERCENT - 1) /
      WMP_INDEX_MAX_FILL_PERCENT;
  uint32_t num_buckets = WMP_INDEX_MIN_BUCKETS;
  while ((uint64_t)num_buckets * WMP_INDEX_BUCKET_SLOTS < min_slots) {
    num_buckets <<= 1;
  }
  const size_t slots_size =
      (size_t)num_buckets * WMP_INDEX_BUCKET_SLOTS * sizeof(WMPEntry);
  void *slots = NULL;
  if (portable_aligned_alloc(&slots, WMP_INDEX_BUCKET_ALIGNMENT, slots_size) !=
      0) {
    log_fatal("could not allocate the wmp word index");
  }
  memset(slots, 0, slots_size);
  wfl->word_index_slots = (WMPEntry *)slots;
  wfl->word_index_bucket_mask = num_buckets - 1;
  wfl->word_index_shift = wmp_index_shift_for_power_of_2(num_buckets);

  uint32_t num_filter_words = WMP_FILTER_MIN_WORDS;
  while ((uint64_t)num_filter_words * 64 <
         (uint64_t)num_entries * WMP_FILTER_BITS_PER_ENTRY) {
    num_filter_words <<= 1;
  }
  wfl->word_filter = calloc_or_die(num_filter_words, sizeof(uint64_t));
  wfl->word_filter_shift = wmp_index_shift_for_power_of_2(num_filter_words);

  for (uint32_t entry_idx = 0; entry_idx < num_entries; entry_idx++) {
    const WMPEntry *entry = &wfl->word_map_entries[entry_idx];
    const BitRack key = wmp_entry_read_bit_rack(entry);
    const uint64_t hash = wmp_index_hash(&key);
    wfl->word_filter[hash >> wfl->word_filter_shift] |=
        wmp_filter_word_bits(hash, wfl->word_filter_shift);
    uint32_t bucket_idx = (uint32_t)(hash >> wfl->word_index_shift);
    bool placed = false;
    while (!placed) {
      WMPEntry *bucket =
          &wfl->word_index_slots[(size_t)bucket_idx * WMP_INDEX_BUCKET_SLOTS];
      for (int slot_idx = 0; slot_idx < WMP_INDEX_BUCKET_SLOTS; slot_idx++) {
        if (wmp_entry_key_is_empty(&bucket[slot_idx])) {
          bucket[slot_idx] = *entry;
          placed = true;
          break;
        }
      }
      bucket_idx = (bucket_idx + 1) & wfl->word_index_bucket_mask;
    }
  }
}

static inline void read_wmp_for_length(WMP *wmp, uint32_t len, FILE *stream) {
  WMPForLength *wfl = &wmp->wfls[len];
  read_wfl_blankless_words(wfl, len, stream);
  read_wfl_blanks(wfl, stream);
  read_wfl_double_blanks(wfl, stream);
  wfl_build_word_index(wfl);
}

static inline void wmp_load_from_filename_with_stream(WMP *wmp,
                                                      const char *wmp_name,
                                                      const char *wmp_filename,
                                                      FILE *stream,
                                                      ErrorStack *error_stack) {
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

static inline void wmp_load_from_filename(WMP *wmp, const char *wmp_name,
                                          const char *wmp_filename,
                                          ErrorStack *error_stack) {
  FILE *stream = stream_from_filename(wmp_filename, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  wmp_load_from_filename_with_stream(wmp, wmp_name, wmp_filename, stream,
                                     error_stack);
  fclose_or_die(stream);
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

    portable_aligned_free(wfl->word_index_slots);
    free(wfl->word_filter);
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

// Returns a pointer to the contiguous word list for a blankless entry
// (no copying) and writes the word count to num_words_out. Valid for
// entries whose subrack contains no blanks; blank entries assemble their
// words from multiple lookups and require a caller buffer instead.
static inline const MachineLetter *
wmp_entry_get_blankless_words(const WMPEntry *entry, const WMPForLength *wfl,
                              int word_length, int *num_words_out) {
  if (wmp_entry_is_inlined(entry)) {
    *num_words_out =
        wmp_entry_number_of_inlined_bytes(entry, word_length) / word_length;
    return entry->bucket_or_inline;
  }
  *num_words_out = (int)entry->num_words;
  return wfl->word_letters + entry->word_start;
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

// Write full BitRack to WMPEntry
static inline void wmp_entry_write_bit_rack(WMPEntry *entry,
                                            const BitRack *bit_rack) {
#if IS_LITTLE_ENDIAN
  memcpy(entry->bit_rack_bytes, bit_rack, WMP_BITRACK_BYTES);
#else
  // Handle big-endian case
  uint64_t low = bit_rack_get_low_64(bit_rack);
  uint64_t high = bit_rack_get_high_64(bit_rack);
  low = htole64(low);
  high = htole64(high);
  memcpy(entry->bit_rack_bytes, &low, 8);
  memcpy(entry->bit_rack_bytes + 8, &high, 8);
#endif
}

// bits must be nonzero.
static inline int wmp_lowest_set_bit(uint32_t bits) {
#if defined(__has_builtin) && __has_builtin(__builtin_ctz)
  return __builtin_ctz(bits);
#else
  int bit_idx = 0;
  while ((bits & 1U) == 0) {
    bits >>= 1;
    bit_idx++;
  }
  return bit_idx;
#endif
}

// Probes the index from the hash's bucket. All of a bucket's slots are
// compared before branching on the result.
static inline const WMPEntry *wfl_index_get_entry(const WMPForLength *wfl,
                                                  uint64_t hash,
                                                  const BitRack *bit_rack) {
  const uint64_t low = bit_rack_get_low_64(bit_rack);
  const uint64_t high = bit_rack_get_high_64(bit_rack);
  uint32_t bucket_idx = (uint32_t)(hash >> wfl->word_index_shift);
  while (true) {
    const WMPEntry *bucket =
        &wfl->word_index_slots[(size_t)bucket_idx * WMP_INDEX_BUCKET_SLOTS];
    uint32_t matching_slots = 0;
    uint32_t empty_slots = 0;
    for (int slot_idx = 0; slot_idx < WMP_INDEX_BUCKET_SLOTS; slot_idx++) {
      const BitRack key = wmp_entry_read_bit_rack(&bucket[slot_idx]);
      const uint64_t key_low = bit_rack_get_low_64(&key);
      const uint64_t key_high = bit_rack_get_high_64(&key);
      matching_slots |= (uint32_t)(((key_low ^ low) | (key_high ^ high)) == 0)
                        << slot_idx;
      empty_slots |= (uint32_t)((key_low | key_high) == 0) << slot_idx;
    }
    if (matching_slots != 0) {
      return &bucket[wmp_lowest_set_bit(matching_slots)];
    }
    if (empty_slots != 0) {
      return NULL;
    }
    bucket_idx = (bucket_idx + 1) & wfl->word_index_bucket_mask;
  }
}

static inline bool wfl_filter_may_contain(const WMPForLength *wfl,
                                          uint64_t hash) {
  const uint64_t word = wfl->word_filter[hash >> wfl->word_filter_shift];
  const uint64_t bits = wmp_filter_word_bits(hash, wfl->word_filter_shift);
  return (word & bits) == bits;
}

static inline const WMPEntry *wfl_get_word_entry(const WMPForLength *wfl,
                                                 const BitRack *bit_rack) {
  const uint64_t hash = wmp_index_hash(bit_rack);
  if (!wfl_filter_may_contain(wfl, hash)) {
    return NULL;
  }
  return wfl_index_get_entry(wfl, hash, bit_rack);
}

// For a rack whose words are known to exist, the filter would only add a
// load.
static inline const WMPEntry *
wfl_get_present_word_entry(const WMPForLength *wfl, const BitRack *bit_rack) {
  return wfl_index_get_entry(wfl, wmp_index_hash(bit_rack), bit_rack);
}

// Called for the letters a blank entry says complete a word, so the
// blankless rack is known to be present.
static inline int wfl_write_blankless_words_to_buffer(const WMPForLength *wfl,
                                                      const BitRack *bit_rack,
                                                      int word_length,
                                                      uint8_t *buffer) {
  const WMPEntry *entry = wfl_get_present_word_entry(wfl, bit_rack);
  if (entry == NULL) {
    return 0;
  }
  return wmp_entry_write_blankless_words_to_buffer(entry, wfl, word_length,
                                                   buffer);
}

// Writes the words for each letter at or above min_ml that the blank
// designates in the entry, in letter order. Each word rack is built in a
// copy, so *bit_rack is never written.
static inline int
wmp_entry_write_blanks_to_buffer(const WMPEntry *entry, const WMPForLength *wfl,
                                 const BitRack *bit_rack, int word_length,
                                 MachineLetter min_ml, uint8_t *buffer) {
  BitRack blankless_rack = *bit_rack;
  bit_rack_set_letter_count(&blankless_rack, BLANK_MACHINE_LETTER, 0);
  uint32_t letters = entry->blank_letters & ~((1U << min_ml) - 1U);
  int bytes_written = 0;
  while (letters != 0) {
    const MachineLetter ml = (MachineLetter)wmp_lowest_set_bit(letters);
    letters &= letters - 1U;
    BitRack word_rack = blankless_rack;
    bit_rack_add_letter(&word_rack, ml);
    bytes_written += wfl_write_blankless_words_to_buffer(
        wfl, &word_rack, word_length, buffer + bytes_written);
  }
  return bytes_written;
}

static inline const WMPEntry *wfl_get_blank_entry(const WMPForLength *wfl,
                                                  const BitRack *bit_rack) {
  if (wfl->num_blank_buckets == 0) {
    return NULL;
  }
  const uint32_t bucket_index =
      bit_rack_get_bucket_index(bit_rack, wfl->num_blank_buckets);
  const uint32_t start = wfl->blank_bucket_starts[bucket_index];
  const uint32_t end = wfl->blank_bucket_starts[bucket_index + 1];
  for (uint32_t i = start; i < end; i++) {
    const WMPEntry *entry = &wfl->blank_map_entries[i];
    const BitRack entry_bit_rack = wmp_entry_read_bit_rack(entry);
    if (bit_rack_equals(&entry_bit_rack, bit_rack)) {
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
  const uint32_t bucket_index =
      bit_rack_get_bucket_index(bit_rack, wfl->num_double_blank_buckets);
  const uint32_t start = wfl->double_blank_bucket_starts[bucket_index];
  const uint32_t end = wfl->double_blank_bucket_starts[bucket_index + 1];
  for (uint32_t i = start; i < end; i++) {
    const WMPEntry *entry = &wfl->double_blank_map_entries[i];
    const BitRack entry_bit_rack = wmp_entry_read_bit_rack(entry);
    if (bit_rack_equals(&entry_bit_rack, bit_rack)) {
      return entry;
    }
  }
  return NULL;
}

static inline const char *wmp_get_name(const WMP *wmp) { return wmp->name; }

// A value that is unique to this loaded WMP instance: a hash of the base
// addresses and entry counts of its freshly-allocated internal maps. Two
// different loads of even the same lexicon produce different fingerprints
// (their maps are malloc'd separately), so callers that cache pointers into a
// WMP (e.g. the move_gen subrack cache) can detect that the WMP backing those
// pointers has been replaced -- even when a new WMP is allocated at the same
// struct address as a freed one (ABA), which a pointer comparison cannot see.
static inline uint64_t wmp_get_instance_fingerprint(const WMP *wmp) {
  uint64_t hash = FNV_64_OFFSET_BASIS;
  // Only lengths 2..board_dim hold loaded map data (see read_wmp_for_length);
  // wfls[0]/wfls[1] are never populated and are indeterminate for WMPs built
  // with make_wmp_from_words (which mallocs the struct), so skip them rather
  // than hash uninitialized memory. board_dim is validated to equal BOARD_DIM
  // at load.
  for (int len = 2; len <= wmp->board_dim; len++) {
    const WMPForLength *wfl = &wmp->wfls[len];
    hash = fnv64a_step(hash, (uintptr_t)wfl->word_map_entries);
    hash = fnv64a_step(hash, (uint64_t)wfl->num_word_entries);
    hash = fnv64a_step(hash, (uintptr_t)wfl->blank_map_entries);
    hash = fnv64a_step(hash, (uint64_t)wfl->num_blank_entries);
    hash = fnv64a_step(hash, (uintptr_t)wfl->double_blank_map_entries);
    hash = fnv64a_step(hash, (uint64_t)wfl->num_double_blank_entries);
  }
  return hash;
}

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
                                             const BitRack *bit_rack,
                                             int word_length,
                                             MachineLetter min_ml,
                                             uint8_t *buffer) {
  const WMPEntry *entry = wfl_get_blank_entry(wfl, bit_rack);
  if (entry == NULL) {
    return 0;
  }
  return wmp_entry_write_blanks_to_buffer(entry, wfl, bit_rack, word_length,
                                          min_ml, buffer);
}

// Expands the first blank over the letters the entry designates, in letter
// order, and the second over the same letter or later ones. Each one-blank
// rack is built in a copy, so *bit_rack is never written.
static inline int wmp_entry_write_double_blanks_to_buffer(
    const WMPEntry *entry, const WMPForLength *wfl, const BitRack *bit_rack,
    int word_length, uint8_t *buffer) {
  BitRack one_blank_rack = *bit_rack;
  bit_rack_set_letter_count(&one_blank_rack, BLANK_MACHINE_LETTER, 1);
  // Bit 0 would be the blank itself.
  uint32_t letters = entry->first_blank_letters & ~1U;
  int bytes_written = 0;
  while (letters != 0) {
    const MachineLetter ml = (MachineLetter)wmp_lowest_set_bit(letters);
    letters &= letters - 1U;
    BitRack blank_rack = one_blank_rack;
    bit_rack_add_letter(&blank_rack, ml);
    bytes_written += wfl_write_blanks_to_buffer(wfl, &blank_rack, word_length,
                                                ml, buffer + bytes_written);
  }
  return bytes_written;
}

static inline int wmp_entry_write_words_to_buffer(const WMPEntry *entry,
                                                  const WMP *wmp,
                                                  const BitRack *bit_rack,
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

static inline int wmp_write_words_to_buffer(const WMP *wmp,
                                            const BitRack *bit_rack,
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