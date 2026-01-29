#include "wmp_maker.h"

#include "../compat/cpthread.h"
#include "../def/bit_rack_defs.h"
#include "../def/board_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/wmp_defs.h"
#include "../ent/bit_rack.h"
#include "../ent/dictionary_word.h"
#include "../ent/letter_distribution.h"
#include "../ent/wmp.h"
#include "../util/io_util.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Utility functions
// ============================================================================

// Round up to next power of 2
static inline uint32_t next_power_of_2(uint32_t n) {
  if (n == 0) return 1;
  n--;
  n |= n >> 1;
  n |= n >> 2;
  n |= n >> 4;
  n |= n >> 8;
  n |= n >> 16;
  return n + 1;
}

// Count trailing zeros - returns position of lowest set bit
static inline int bit_ctz32(uint32_t x) {
#if defined(__has_builtin) && __has_builtin(__builtin_ctz)
  return __builtin_ctz(x);
#else
  int index = 0;
  while ((x & 1) == 0) {
    x >>= 1;
    index++;
  }
  return index;
#endif
}

// Get a bitmask of which letters (0-31) are present in the BitRack
static inline uint32_t bit_rack_get_letter_mask(const BitRack *bit_rack) {
  uint64_t low = bit_rack_get_low_64(bit_rack);
  uint64_t high = bit_rack_get_high_64(bit_rack);
  uint32_t mask = 0;
  for (int i = 0; i < 16; i++) {
    if (low & 0xF) mask |= 1U << i;
    low >>= 4;
  }
  for (int i = 0; i < 16; i++) {
    if (high & 0xF) mask |= 1U << (i + 16);
    high >>= 4;
  }
  return mask;
}

// ============================================================================
// Sort pair structures
// ============================================================================

typedef struct WordPair {
  BitRack bit_rack;
  uint32_t word_index;
} WordPair;

typedef struct BlankPair {
  BitRack bit_rack;
  uint32_t blank_letter_bit;
} BlankPair;

typedef struct DoubleBlankPair {
  BitRack bit_rack;
  uint16_t packed_pair;  // ml1 | (ml2 << 8)
} DoubleBlankPair;

// ============================================================================
// Radix sort implementations (16-byte BitRack keys)
// ============================================================================

static void radix_pass_word_pairs(WordPair *src, WordPair *dst, uint32_t count,
                                   int byte_idx) {
  uint32_t counts[256] = {0};
  for (uint32_t i = 0; i < count; i++) {
    uint8_t byte;
    if (byte_idx < 8) {
      byte = (bit_rack_get_low_64(&src[i].bit_rack) >> (byte_idx * 8)) & 0xFF;
    } else {
      byte = (bit_rack_get_high_64(&src[i].bit_rack) >> ((byte_idx - 8) * 8)) & 0xFF;
    }
    counts[byte]++;
  }
  uint32_t total = 0;
  for (int b = 0; b < 256; b++) {
    uint32_t c = counts[b];
    counts[b] = total;
    total += c;
  }
  for (uint32_t i = 0; i < count; i++) {
    uint8_t byte;
    if (byte_idx < 8) {
      byte = (bit_rack_get_low_64(&src[i].bit_rack) >> (byte_idx * 8)) & 0xFF;
    } else {
      byte = (bit_rack_get_high_64(&src[i].bit_rack) >> ((byte_idx - 8) * 8)) & 0xFF;
    }
    dst[counts[byte]++] = src[i];
  }
}

static void radix_sort_word_pairs(WordPair *pairs, WordPair *temp, uint32_t count) {
  if (count <= 1) return;
  for (int pass = 0; pass < 16; pass++) {
    if (pass % 2 == 0) {
      radix_pass_word_pairs(pairs, temp, count, pass);
    } else {
      radix_pass_word_pairs(temp, pairs, count, pass);
    }
  }
}

static void radix_pass_blank_pairs(BlankPair *src, BlankPair *dst, uint32_t count,
                                    int byte_idx) {
  uint32_t counts[256] = {0};
  for (uint32_t i = 0; i < count; i++) {
    uint8_t byte;
    if (byte_idx < 8) {
      byte = (bit_rack_get_low_64(&src[i].bit_rack) >> (byte_idx * 8)) & 0xFF;
    } else {
      byte = (bit_rack_get_high_64(&src[i].bit_rack) >> ((byte_idx - 8) * 8)) & 0xFF;
    }
    counts[byte]++;
  }
  uint32_t total = 0;
  for (int b = 0; b < 256; b++) {
    uint32_t c = counts[b];
    counts[b] = total;
    total += c;
  }
  for (uint32_t i = 0; i < count; i++) {
    uint8_t byte;
    if (byte_idx < 8) {
      byte = (bit_rack_get_low_64(&src[i].bit_rack) >> (byte_idx * 8)) & 0xFF;
    } else {
      byte = (bit_rack_get_high_64(&src[i].bit_rack) >> ((byte_idx - 8) * 8)) & 0xFF;
    }
    dst[counts[byte]++] = src[i];
  }
}

static void radix_sort_blank_pairs(BlankPair *pairs, BlankPair *temp, uint32_t count) {
  if (count <= 1) return;
  for (int pass = 0; pass < 16; pass++) {
    if (pass % 2 == 0) {
      radix_pass_blank_pairs(pairs, temp, count, pass);
    } else {
      radix_pass_blank_pairs(temp, pairs, count, pass);
    }
  }
}

static void radix_pass_double_blank_pairs(DoubleBlankPair *src, DoubleBlankPair *dst,
                                           uint32_t count, int byte_idx) {
  uint32_t counts[256] = {0};
  for (uint32_t i = 0; i < count; i++) {
    uint8_t byte;
    if (byte_idx < 2) {
      byte = (src[i].packed_pair >> (byte_idx * 8)) & 0xFF;
    } else if (byte_idx < 10) {
      byte = (bit_rack_get_low_64(&src[i].bit_rack) >> ((byte_idx - 2) * 8)) & 0xFF;
    } else {
      byte = (bit_rack_get_high_64(&src[i].bit_rack) >> ((byte_idx - 10) * 8)) & 0xFF;
    }
    counts[byte]++;
  }
  uint32_t total = 0;
  for (int b = 0; b < 256; b++) {
    uint32_t c = counts[b];
    counts[b] = total;
    total += c;
  }
  for (uint32_t i = 0; i < count; i++) {
    uint8_t byte;
    if (byte_idx < 2) {
      byte = (src[i].packed_pair >> (byte_idx * 8)) & 0xFF;
    } else if (byte_idx < 10) {
      byte = (bit_rack_get_low_64(&src[i].bit_rack) >> ((byte_idx - 2) * 8)) & 0xFF;
    } else {
      byte = (bit_rack_get_high_64(&src[i].bit_rack) >> ((byte_idx - 10) * 8)) & 0xFF;
    }
    dst[counts[byte]++] = src[i];
  }
}

static void radix_sort_double_blank_pairs(DoubleBlankPair *pairs, DoubleBlankPair *temp,
                                           uint32_t count) {
  if (count <= 1) return;
  // 18 passes: 2 for packed_pair + 16 for BitRack
  for (int pass = 0; pass < 18; pass++) {
    if (pass % 2 == 0) {
      radix_pass_double_blank_pairs(pairs, temp, count, pass);
    } else {
      radix_pass_double_blank_pairs(temp, pairs, count, pass);
    }
  }
}

// ============================================================================
// Direct WMPForLength construction - Blank entries
// ============================================================================

typedef struct {
  // Input: unique BitRacks from word map
  BitRack *unique_racks;
  uint32_t num_unique_racks;
  WMPForLength *wfl;
  int length;
} BlankBuildArg;

static void *build_blank_entries_direct(void *arg) {
  BlankBuildArg *a = (BlankBuildArg *)arg;
  BitRack *unique_racks = a->unique_racks;
  uint32_t num_racks = a->num_unique_racks;
  WMPForLength *wfl = a->wfl;

  // Estimate max pairs: each rack has up to ~length distinct letters
  uint32_t max_pairs = num_racks * a->length;
  BlankPair *pairs = malloc_or_die(sizeof(BlankPair) * max_pairs);
  BlankPair *temp = malloc_or_die(sizeof(BlankPair) * max_pairs);
  uint32_t num_pairs = 0;

  // Generate all (bit_rack_with_blank, letter_bit) pairs
  for (uint32_t r = 0; r < num_racks; r++) {
    BitRack rack = unique_racks[r];
    uint32_t present = bit_rack_get_letter_mask(&rack) & ~1U;
    while (present) {
      MachineLetter ml = (MachineLetter)bit_ctz32(present);
      present &= present - 1;
      bit_rack_take_letter(&rack, ml);
      bit_rack_add_letter(&rack, BLANK_MACHINE_LETTER);
      pairs[num_pairs].bit_rack = rack;
      pairs[num_pairs].blank_letter_bit = 1U << ml;
      num_pairs++;
      bit_rack_take_letter(&rack, BLANK_MACHINE_LETTER);
      bit_rack_add_letter(&rack, ml);
    }
  }

  // Sort
  radix_sort_blank_pairs(pairs, temp, num_pairs);

  // Count unique BitRacks after merge
  uint32_t num_unique = 0;
  if (num_pairs > 0) {
    num_unique = 1;
    for (uint32_t i = 1; i < num_pairs; i++) {
      if (!bit_rack_equals(&pairs[i].bit_rack, &pairs[i - 1].bit_rack)) {
        num_unique++;
      }
    }
  }

  // Determine bucket count
  wfl->num_blank_buckets = next_power_of_2(num_unique);
  if (wfl->num_blank_buckets < 16) wfl->num_blank_buckets = 16;

  // Count entries per bucket
  uint32_t *bucket_counts = calloc(wfl->num_blank_buckets, sizeof(uint32_t));
  if (!bucket_counts) log_fatal("calloc failed");

  if (num_pairs > 0) {
    BitRack *prev = &pairs[0].bit_rack;
    bucket_counts[bit_rack_get_bucket_index(prev, wfl->num_blank_buckets)]++;
    for (uint32_t i = 1; i < num_pairs; i++) {
      if (!bit_rack_equals(&pairs[i].bit_rack, prev)) {
        prev = &pairs[i].bit_rack;
        bucket_counts[bit_rack_get_bucket_index(prev, wfl->num_blank_buckets)]++;
      }
    }
  }

  // Allocate final arrays
  wfl->num_blank_entries = num_unique;
  wfl->blank_map_entries = malloc_or_die(num_unique * sizeof(WMPEntry));
  wfl->blank_bucket_starts = malloc_or_die((wfl->num_blank_buckets + 1) * sizeof(uint32_t));

  // Compute bucket starts
  uint32_t offset = 0;
  for (uint32_t b = 0; b < wfl->num_blank_buckets; b++) {
    wfl->blank_bucket_starts[b] = offset;
    offset += bucket_counts[b];
  }
  wfl->blank_bucket_starts[wfl->num_blank_buckets] = offset;

  // Reset for insertion
  memset(bucket_counts, 0, wfl->num_blank_buckets * sizeof(uint32_t));

  // Write entries directly (merge consecutive same-BitRack pairs)
  if (num_pairs > 0) {
    BitRack current_rack = pairs[0].bit_rack;
    uint32_t current_bits = pairs[0].blank_letter_bit;

    for (uint32_t i = 1; i <= num_pairs; i++) {
      bool is_end = (i == num_pairs) || !bit_rack_equals(&pairs[i].bit_rack, &current_rack);
      if (!is_end) {
        current_bits |= pairs[i].blank_letter_bit;
      } else {
        // Emit entry
        uint32_t bucket_idx = bit_rack_get_bucket_index(&current_rack, wfl->num_blank_buckets);
        uint32_t entry_idx = wfl->blank_bucket_starts[bucket_idx] + bucket_counts[bucket_idx]++;
        WMPEntry *entry = &wfl->blank_map_entries[entry_idx];
        memset(entry->bucket_or_inline, 0, WMP_INLINE_VALUE_BYTES);
        entry->blank_letters = current_bits;
        wmp_entry_write_bit_rack(entry, &current_rack);

        if (i < num_pairs) {
          current_rack = pairs[i].bit_rack;
          current_bits = pairs[i].blank_letter_bit;
        }
      }
    }
  }

  free(bucket_counts);
  free(pairs);
  free(temp);
  free(unique_racks);
  return NULL;
}

// ============================================================================
// Direct WMPForLength construction - Double blank entries
// ============================================================================

typedef struct {
  BitRack *unique_racks;
  uint32_t num_unique_racks;
  WMPForLength *wfl;
  int length;
} DoubleBlankBuildArg;

static void *build_double_blank_entries_direct(void *arg) {
  DoubleBlankBuildArg *a = (DoubleBlankBuildArg *)arg;
  BitRack *unique_racks = a->unique_racks;
  uint32_t num_racks = a->num_unique_racks;
  WMPForLength *wfl = a->wfl;
  int length = a->length;

  // Estimate max pairs
  uint32_t max_pairs = num_racks * (uint32_t)length * (uint32_t)length / 2;
  DoubleBlankPair *pairs = malloc_or_die(sizeof(DoubleBlankPair) * max_pairs);
  DoubleBlankPair *temp = malloc_or_die(sizeof(DoubleBlankPair) * max_pairs);
  uint32_t num_pairs = 0;

  // Generate all (bit_rack_with_two_blanks, packed_pair) pairs
  for (uint32_t r = 0; r < num_racks; r++) {
    BitRack rack = unique_racks[r];
    uint32_t present1 = bit_rack_get_letter_mask(&rack) & ~1U;
    while (present1) {
      MachineLetter ml1 = (MachineLetter)bit_ctz32(present1);
      present1 &= present1 - 1;
      bit_rack_take_letter(&rack, ml1);
      bit_rack_add_letter(&rack, BLANK_MACHINE_LETTER);

      uint32_t present2 = bit_rack_get_letter_mask(&rack) & ~((1U << ml1) - 1) & ~1U;
      while (present2) {
        MachineLetter ml2 = (MachineLetter)bit_ctz32(present2);
        present2 &= present2 - 1;
        bit_rack_take_letter(&rack, ml2);
        bit_rack_add_letter(&rack, BLANK_MACHINE_LETTER);

        pairs[num_pairs].bit_rack = rack;
        pairs[num_pairs].packed_pair = (uint16_t)ml1 | ((uint16_t)ml2 << 8);
        num_pairs++;

        bit_rack_take_letter(&rack, BLANK_MACHINE_LETTER);
        bit_rack_add_letter(&rack, ml2);
      }

      bit_rack_take_letter(&rack, BLANK_MACHINE_LETTER);
      bit_rack_add_letter(&rack, ml1);
    }
  }

  // Sort by (BitRack, packed_pair)
  radix_sort_double_blank_pairs(pairs, temp, num_pairs);

  // Count unique BitRacks
  uint32_t num_unique = 0;
  if (num_pairs > 0) {
    num_unique = 1;
    for (uint32_t i = 1; i < num_pairs; i++) {
      if (!bit_rack_equals(&pairs[i].bit_rack, &pairs[i - 1].bit_rack)) {
        num_unique++;
      }
    }
  }

  // Determine bucket count
  wfl->num_double_blank_buckets = next_power_of_2(num_unique);
  if (wfl->num_double_blank_buckets < 16) wfl->num_double_blank_buckets = 16;

  // Count entries per bucket
  uint32_t *bucket_counts = calloc(wfl->num_double_blank_buckets, sizeof(uint32_t));
  if (!bucket_counts) log_fatal("calloc failed");

  if (num_pairs > 0) {
    BitRack *prev = &pairs[0].bit_rack;
    bucket_counts[bit_rack_get_bucket_index(prev, wfl->num_double_blank_buckets)]++;
    for (uint32_t i = 1; i < num_pairs; i++) {
      if (!bit_rack_equals(&pairs[i].bit_rack, prev)) {
        prev = &pairs[i].bit_rack;
        bucket_counts[bit_rack_get_bucket_index(prev, wfl->num_double_blank_buckets)]++;
      }
    }
  }

  // Allocate final arrays
  wfl->num_double_blank_entries = num_unique;
  wfl->double_blank_map_entries = malloc_or_die(num_unique * sizeof(WMPEntry));
  wfl->double_blank_bucket_starts = malloc_or_die((wfl->num_double_blank_buckets + 1) * sizeof(uint32_t));

  // Compute bucket starts
  uint32_t offset = 0;
  for (uint32_t b = 0; b < wfl->num_double_blank_buckets; b++) {
    wfl->double_blank_bucket_starts[b] = offset;
    offset += bucket_counts[b];
  }
  wfl->double_blank_bucket_starts[wfl->num_double_blank_buckets] = offset;

  // Reset for insertion
  memset(bucket_counts, 0, wfl->num_double_blank_buckets * sizeof(uint32_t));

  // Write entries directly - compute first_blank_letters for each unique BitRack
  if (num_pairs > 0) {
    uint32_t run_start = 0;
    for (uint32_t i = 1; i <= num_pairs; i++) {
      bool is_end = (i == num_pairs) || !bit_rack_equals(&pairs[i].bit_rack, &pairs[i - 1].bit_rack);
      if (is_end) {
        BitRack rack = pairs[run_start].bit_rack;
        // Compute first_blank_letters: OR of all first letters in pairs
        uint32_t first_blank_letters = 0;
        uint16_t last_pair = 0xFFFF;
        for (uint32_t j = run_start; j < i; j++) {
          if (pairs[j].packed_pair != last_pair) {
            first_blank_letters |= 1U << (pairs[j].packed_pair & 0xFF);
            last_pair = pairs[j].packed_pair;
          }
        }

        uint32_t bucket_idx = bit_rack_get_bucket_index(&rack, wfl->num_double_blank_buckets);
        uint32_t entry_idx = wfl->double_blank_bucket_starts[bucket_idx] + bucket_counts[bucket_idx]++;
        WMPEntry *entry = &wfl->double_blank_map_entries[entry_idx];
        memset(entry->bucket_or_inline, 0, WMP_INLINE_VALUE_BYTES);
        entry->first_blank_letters = first_blank_letters;
        wmp_entry_write_bit_rack(entry, &rack);

        run_start = i;
      }
    }
  }

  free(bucket_counts);
  free(pairs);
  free(temp);
  free(unique_racks);
  return NULL;
}

// ============================================================================
// Thread arguments for parallel construction
// ============================================================================

typedef struct {
  const DictionaryWordList *words;
  WMPForLength *wfl;
  int length;
  // For word build phase
  WordPair *pairs;
  WordPair *temp;
  uint32_t pair_count;
  // Output: unique racks for blank/double-blank phases
  BitRack *unique_racks;
  uint32_t num_unique_racks;
} LengthBuildContext;

// Build word map and extract unique racks
static void *build_words_and_extract_racks(void *arg) {
  LengthBuildContext *ctx = (LengthBuildContext *)arg;
  WordPair *pairs = ctx->pairs;
  WordPair *temp = ctx->temp;
  uint32_t count = ctx->pair_count;
  WMPForLength *wfl = ctx->wfl;
  const DictionaryWordList *words = ctx->words;
  const int word_length = ctx->length;

  // Sort pairs
  radix_sort_word_pairs(pairs, temp, count);

  // Count unique bit_racks
  uint32_t num_unique = 0;
  if (count > 0) {
    num_unique = 1;
    for (uint32_t i = 1; i < count; i++) {
      if (!bit_rack_equals(&pairs[i].bit_rack, &pairs[i - 1].bit_rack)) {
        num_unique++;
      }
    }
  }

  // Extract unique racks for blank/double-blank phases
  ctx->num_unique_racks = num_unique;
  ctx->unique_racks = malloc_or_die(sizeof(BitRack) * (num_unique > 0 ? num_unique : 1));
  if (count > 0) {
    uint32_t rack_idx = 0;
    ctx->unique_racks[rack_idx++] = pairs[0].bit_rack;
    for (uint32_t i = 1; i < count; i++) {
      if (!bit_rack_equals(&pairs[i].bit_rack, &pairs[i - 1].bit_rack)) {
        ctx->unique_racks[rack_idx++] = pairs[i].bit_rack;
      }
    }
  }

  // Now build word entries directly
  wfl->num_word_buckets = next_power_of_2(num_unique);
  if (wfl->num_word_buckets < 16) wfl->num_word_buckets = 16;

  uint32_t *bucket_counts = calloc(wfl->num_word_buckets, sizeof(uint32_t));
  if (!bucket_counts) log_fatal("calloc failed");

  const uint32_t max_inline = max_inlined_words(word_length);
  uint32_t num_uninlined_letters = 0;

  if (count > 0) {
    uint32_t run_start = 0;
    for (uint32_t i = 1; i <= count; i++) {
      bool is_end = (i == count) || !bit_rack_equals(&pairs[i].bit_rack, &pairs[i - 1].bit_rack);
      if (is_end) {
        uint32_t words_in_run = i - run_start;
        bucket_counts[bit_rack_get_bucket_index(&pairs[run_start].bit_rack, wfl->num_word_buckets)]++;
        if (words_in_run > max_inline) {
          num_uninlined_letters += words_in_run * word_length;
        }
        run_start = i;
      }
    }
  }

  wfl->num_word_entries = num_unique;
  wfl->word_map_entries = malloc_or_die(num_unique * sizeof(WMPEntry));
  wfl->num_uninlined_words = num_uninlined_letters / word_length;
  wfl->word_letters = malloc_or_die(num_uninlined_letters > 0 ? num_uninlined_letters : 1);
  wfl->word_bucket_starts = malloc_or_die((wfl->num_word_buckets + 1) * sizeof(uint32_t));

  uint32_t offset = 0;
  for (uint32_t b = 0; b < wfl->num_word_buckets; b++) {
    wfl->word_bucket_starts[b] = offset;
    offset += bucket_counts[b];
  }
  wfl->word_bucket_starts[wfl->num_word_buckets] = offset;

  memset(bucket_counts, 0, wfl->num_word_buckets * sizeof(uint32_t));

  uint32_t letter_offset = 0;
  if (count > 0) {
    uint32_t run_start = 0;
    for (uint32_t i = 1; i <= count; i++) {
      bool is_end = (i == count) || !bit_rack_equals(&pairs[i].bit_rack, &pairs[i - 1].bit_rack);
      if (is_end) {
        uint32_t words_in_run = i - run_start;
        BitRack rack = pairs[run_start].bit_rack;
        uint32_t bucket_idx = bit_rack_get_bucket_index(&rack, wfl->num_word_buckets);
        uint32_t entry_idx = wfl->word_bucket_starts[bucket_idx] + bucket_counts[bucket_idx]++;

        WMPEntry *entry = &wfl->word_map_entries[entry_idx];
        wmp_entry_write_bit_rack(entry, &rack);

        if (words_in_run <= max_inline) {
          memset(entry->bucket_or_inline, 0, WMP_INLINE_VALUE_BYTES);
          for (uint32_t j = 0; j < words_in_run; j++) {
            const DictionaryWord *word = dictionary_word_list_get_word(words, (int)pairs[run_start + j].word_index);
            memcpy(entry->bucket_or_inline + j * word_length,
                   dictionary_word_get_word(word), word_length);
          }
        } else {
          memset(entry->bucket_or_inline, 0, WMP_INLINE_VALUE_BYTES);
          entry->word_start = letter_offset;
          entry->num_words = words_in_run;
          for (uint32_t j = 0; j < words_in_run; j++) {
            const DictionaryWord *word = dictionary_word_list_get_word(words, (int)pairs[run_start + j].word_index);
            memcpy(wfl->word_letters + letter_offset, dictionary_word_get_word(word), word_length);
            letter_offset += word_length;
          }
        }
        run_start = i;
      }
    }
  }

  free(bucket_counts);
  free(pairs);
  free(temp);
  return NULL;
}

// ============================================================================
// max_word_lookup_bytes calculation (needed for WMP header)
// ============================================================================

// Get word count for a BitRack from the WMPForLength
static uint32_t wfl_get_word_count(const WMPForLength *wfl, const BitRack *bit_rack,
                                    int word_length) {
  uint32_t bucket_idx = bit_rack_get_bucket_index(bit_rack, wfl->num_word_buckets);
  uint32_t start = wfl->word_bucket_starts[bucket_idx];
  uint32_t end = wfl->word_bucket_starts[bucket_idx + 1];
  for (uint32_t i = start; i < end; i++) {
    BitRack entry_rack = wmp_entry_read_bit_rack(&wfl->word_map_entries[i]);
    if (bit_rack_equals(&entry_rack, bit_rack)) {
      const WMPEntry *e = &wfl->word_map_entries[i];
      if (wmp_entry_is_inlined(e)) {
        return wmp_entry_number_of_inlined_bytes(e, word_length) / word_length;
      }
      return e->num_words;
    }
  }
  return 0;
}

// Calculate max_word_lookup_bytes for the WMP
static uint32_t calculate_max_word_lookup_bytes(WMP *wmp) {
  uint32_t max_bytes = 0;
  for (int len = 2; len <= BOARD_DIM; len++) {
    WMPForLength *wfl = &wmp->wfls[len];
    // Check double-blank entries for max words
    for (uint32_t i = 0; i < wfl->num_double_blank_entries; i++) {
      WMPEntry *entry = &wfl->double_blank_map_entries[i];
      BitRack rack = wmp_entry_read_bit_rack(entry);
      uint32_t first_blanks = entry->first_blank_letters;
      uint32_t total_words = 0;

      // For each first blank letter
      bit_rack_set_letter_count(&rack, BLANK_MACHINE_LETTER, 1);
      for (MachineLetter ml1 = 1; ml1 < BIT_RACK_MAX_ALPHABET_SIZE; ml1++) {
        if (!(first_blanks & (1U << ml1))) continue;
        bit_rack_add_letter(&rack, ml1);

        // Look up blank entry for this rack
        uint32_t blank_bucket = bit_rack_get_bucket_index(&rack, wfl->num_blank_buckets);
        uint32_t bstart = wfl->blank_bucket_starts[blank_bucket];
        uint32_t bend = wfl->blank_bucket_starts[blank_bucket + 1];
        for (uint32_t bi = bstart; bi < bend; bi++) {
          BitRack blank_rack = wmp_entry_read_bit_rack(&wfl->blank_map_entries[bi]);
          if (bit_rack_equals(&blank_rack, &rack)) {
            uint32_t second_blanks = wfl->blank_map_entries[bi].blank_letters;
            // Only count letters >= ml1
            for (MachineLetter ml2 = ml1; ml2 < BIT_RACK_MAX_ALPHABET_SIZE; ml2++) {
              if (!(second_blanks & (1U << ml2))) continue;
              bit_rack_add_letter(&rack, ml2);
              bit_rack_set_letter_count(&rack, BLANK_MACHINE_LETTER, 0);
              total_words += wfl_get_word_count(wfl, &rack, len);
              bit_rack_set_letter_count(&rack, BLANK_MACHINE_LETTER, 1);
              bit_rack_take_letter(&rack, ml2);
            }
            break;
          }
        }
        bit_rack_take_letter(&rack, ml1);
      }

      uint32_t bytes = total_words * len;
      if (bytes > max_bytes) max_bytes = bytes;
    }
  }
  return max_bytes;
}

// ============================================================================
// Main entry point
// ============================================================================

WMP *make_wmp_from_words(const DictionaryWordList *words,
                         const LetterDistribution *ld) {
  if (ld->distribution[BLANK_MACHINE_LETTER] > 2) {
    log_fatal("cannot create WMP with more than 2 blanks");
    return NULL;
  }

  const int total_words = dictionary_word_list_get_count(words);

  // Count words by length
  int num_words_by_length[BOARD_DIM + 1] = {0};
  for (int i = 0; i < total_words; i++) {
    const DictionaryWord *word = dictionary_word_list_get_word(words, i);
    num_words_by_length[dictionary_word_get_length(word)]++;
  }

  // Create pairs for each length
  WordPair *pairs_by_length[BOARD_DIM + 1] = {NULL};
  WordPair *temp_by_length[BOARD_DIM + 1] = {NULL};
  uint32_t pair_counts[BOARD_DIM + 1] = {0};

  for (int len = 2; len <= BOARD_DIM; len++) {
    if (num_words_by_length[len] > 0) {
      pairs_by_length[len] = malloc_or_die(sizeof(WordPair) * num_words_by_length[len]);
      temp_by_length[len] = malloc_or_die(sizeof(WordPair) * num_words_by_length[len]);
    }
  }

  // Generate pairs
  for (int i = 0; i < total_words; i++) {
    const DictionaryWord *word = dictionary_word_list_get_word(words, i);
    uint8_t len = dictionary_word_get_length(word);
    uint32_t idx = pair_counts[len]++;
    pairs_by_length[len][idx].bit_rack = bit_rack_create_from_dictionary_word(word);
    pairs_by_length[len][idx].word_index = (uint32_t)i;
  }

  WMP *wmp = malloc_or_die(sizeof(WMP));
  wmp->name = NULL;
  wmp->version = WMP_VERSION;
  wmp->board_dim = BOARD_DIM;

  // Phase 1: Build word entries in parallel and extract unique racks
  LengthBuildContext contexts[BOARD_DIM + 1];
  pthread_t word_threads[BOARD_DIM + 1];

  for (int len = 2; len <= BOARD_DIM; len++) {
    contexts[len].words = words;
    contexts[len].wfl = &wmp->wfls[len];
    contexts[len].length = len;
    contexts[len].pairs = pairs_by_length[len];
    contexts[len].temp = temp_by_length[len];
    contexts[len].pair_count = pair_counts[len];
    contexts[len].unique_racks = NULL;
    contexts[len].num_unique_racks = 0;

    if (pair_counts[len] > 0) {
      cpthread_create(&word_threads[len], build_words_and_extract_racks, &contexts[len]);
    } else {
      // Initialize empty wfl
      WMPForLength *wfl = &wmp->wfls[len];
      wfl->num_word_buckets = 16;
      wfl->num_word_entries = 0;
      wfl->num_uninlined_words = 0;
      wfl->word_bucket_starts = malloc_or_die(17 * sizeof(uint32_t));
      memset(wfl->word_bucket_starts, 0, 17 * sizeof(uint32_t));
      wfl->word_map_entries = malloc_or_die(sizeof(WMPEntry));
      wfl->word_letters = malloc_or_die(1);
      contexts[len].unique_racks = malloc_or_die(sizeof(BitRack));
      contexts[len].num_unique_racks = 0;
    }
  }

  for (int len = 2; len <= BOARD_DIM; len++) {
    if (pair_counts[len] > 0) {
      cpthread_join(word_threads[len]);
    }
  }

  // Phase 2: Build blank entries in parallel
  BlankBuildArg blank_args[BOARD_DIM + 1];
  pthread_t blank_threads[BOARD_DIM + 1];

  for (int len = 2; len <= BOARD_DIM; len++) {
    // Copy unique_racks (will be freed by build_blank_entries_direct)
    uint32_t n = contexts[len].num_unique_racks;
    BitRack *racks_copy = malloc_or_die(sizeof(BitRack) * (n > 0 ? n : 1));
    if (n > 0) memcpy(racks_copy, contexts[len].unique_racks, sizeof(BitRack) * n);

    blank_args[len].unique_racks = racks_copy;
    blank_args[len].num_unique_racks = n;
    blank_args[len].wfl = &wmp->wfls[len];
    blank_args[len].length = len;
    cpthread_create(&blank_threads[len], build_blank_entries_direct, &blank_args[len]);
  }

  for (int len = 2; len <= BOARD_DIM; len++) {
    cpthread_join(blank_threads[len]);
  }

  // Phase 3: Build double-blank entries in parallel
  DoubleBlankBuildArg dbl_args[BOARD_DIM + 1];
  pthread_t dbl_threads[BOARD_DIM + 1];

  for (int len = 2; len <= BOARD_DIM; len++) {
    // unique_racks will be freed by build_double_blank_entries_direct
    dbl_args[len].unique_racks = contexts[len].unique_racks;
    dbl_args[len].num_unique_racks = contexts[len].num_unique_racks;
    dbl_args[len].wfl = &wmp->wfls[len];
    dbl_args[len].length = len;
    cpthread_create(&dbl_threads[len], build_double_blank_entries_direct, &dbl_args[len]);
  }

  for (int len = 2; len <= BOARD_DIM; len++) {
    cpthread_join(dbl_threads[len]);
  }

  // Calculate max_word_lookup_bytes
  wmp->max_word_lookup_bytes = calculate_max_word_lookup_bytes(wmp);

  return wmp;
}
