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

typedef struct MutableWordsMapEntry {
  BitRack bit_rack;
  DictionaryWordList *letters;
} MutableWordMapEntry;

typedef struct MutableWordMapBucket {
  MutableWordMapEntry *entries;
  uint32_t num_entries;
  uint32_t capacity;
} MutableWordMapBucket;

typedef struct MutableWordsOfSameLengthMap {
  MutableWordMapBucket *word_buckets;
  uint32_t num_word_buckets;
} MutableWordsOfSameLengthMap;

typedef struct MutableWordMap {
  MutableWordsOfSameLengthMap maps[BOARD_DIM + 1];
} MutableWordMap;

typedef struct MutableBlankMapEntry {
  BitRack bit_rack;
  uint32_t blank_letters;
} MutableBlankMapEntry;

typedef struct MutableBlankMapBucket {
  MutableBlankMapEntry *entries;
  uint32_t num_entries;
  uint32_t capacity;
} MutableBlankMapBucket;

typedef struct MutableBlanksForSameLengthMap {
  MutableBlankMapBucket *blank_buckets;
  uint32_t num_blank_buckets;
} MutableBlanksForSameLengthMap;

typedef struct MutableBlankMap {
  MutableBlanksForSameLengthMap maps[BOARD_DIM + 1];
} MutableBlankMap;

// ============================================================================
// Bit manipulation helpers for efficient letter iteration
// ============================================================================

// Count trailing zeros - returns position of lowest set bit
static inline int bit_ctz32(uint32_t x) {
#if defined(__has_builtin) && __has_builtin(__builtin_ctz)
  return __builtin_ctz(x);
#else
  // Fallback: simple loop (x is guaranteed non-zero when called)
  int index = 0;
  while ((x & 1) == 0) {
    x >>= 1;
    index++;
  }
  return index;
#endif
}

// Get a bitmask of which letters (0-31) are present in the BitRack
// Bit N is set if letter N has count > 0
static inline uint32_t bit_rack_get_letter_mask(const BitRack *bit_rack) {
  uint64_t low = bit_rack_get_low_64(bit_rack);
  uint64_t high = bit_rack_get_high_64(bit_rack);

  // Simple loop to check each nibble - compiler will unroll it
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
// Radix sort for 128-bit BitRack keys (faster than qsort comparison-based sort)
// ============================================================================

// Forward declaration (defined later in file)
static inline uint32_t next_power_of_2(uint32_t n);

typedef struct BlankPair {
  BitRack bit_rack;
  uint32_t blank_letter_bit;
} BlankPair;

static int compare_blank_pairs(const void *a, const void *b) {
  const BlankPair *pa = (const BlankPair *)a;
  const BlankPair *pb = (const BlankPair *)b;
  uint64_t ah = bit_rack_get_high_64(&pa->bit_rack);
  uint64_t bh = bit_rack_get_high_64(&pb->bit_rack);
  if (ah != bh) return (ah < bh) ? -1 : 1;
  uint64_t al = bit_rack_get_low_64(&pa->bit_rack);
  uint64_t bl = bit_rack_get_low_64(&pb->bit_rack);
  if (al != bl) return (al < bl) ? -1 : 1;
  return 0;
}

static void radix_sort_blank_pairs(BlankPair *pairs, uint32_t count) {
  qsort(pairs, count, sizeof(BlankPair), compare_blank_pairs);
}

// DoubleBlankPair for sort-and-merge double-blank map construction
typedef struct DoubleBlankPair {
  BitRack bit_rack;
  uint16_t packed_pair;
} DoubleBlankPair;

static int compare_double_blank_pairs(const void *a, const void *b) {
  const DoubleBlankPair *pa = (const DoubleBlankPair *)a;
  const DoubleBlankPair *pb = (const DoubleBlankPair *)b;
  uint64_t ah = bit_rack_get_high_64(&pa->bit_rack);
  uint64_t bh = bit_rack_get_high_64(&pb->bit_rack);
  if (ah != bh) return (ah < bh) ? -1 : 1;
  uint64_t al = bit_rack_get_low_64(&pa->bit_rack);
  uint64_t bl = bit_rack_get_low_64(&pb->bit_rack);
  if (al != bl) return (al < bl) ? -1 : 1;
  if (pa->packed_pair != pb->packed_pair) {
    return (pa->packed_pair < pb->packed_pair) ? -1 : 1;
  }
  return 0;
}

static void radix_sort_double_blank_pairs(DoubleBlankPair *pairs, uint32_t count) {
  qsort(pairs, count, sizeof(DoubleBlankPair), compare_double_blank_pairs);
}

// ============================================================================
// Sort-and-merge word map construction
// ============================================================================

// Entry for sort-based word map construction
typedef struct WordPair {
  BitRack bit_rack;
  uint32_t word_index;  // Index into original word list
} WordPair;

// Radix sort for WordPair arrays - single pass helper
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

static void radix_sort_word_pairs(WordPair *pairs, uint32_t count) {
  if (count <= 1) return;

  WordPair *temp = malloc_or_die(sizeof(WordPair) * count);

  for (int pass = 0; pass < 12; pass++) {
    if (pass % 2 == 0) {
      radix_pass_word_pairs(pairs, temp, count, pass);
    } else {
      radix_pass_word_pairs(temp, pairs, count, pass);
    }
  }

  free(temp);
}

// Thread argument for building word map for a single length
typedef struct {
  const DictionaryWordList *words;
  WordPair *pairs;
  uint32_t pair_count;
  MutableWordsOfSameLengthMap *mwfl;
  int length;
} WordMapThreadArg;

// Build word map for a single length (thread function)
static void *build_word_map_for_length(void *arg) {
  WordMapThreadArg *targ = (WordMapThreadArg *)arg;
  WordPair *pairs = targ->pairs;
  uint32_t count = targ->pair_count;
  MutableWordsOfSameLengthMap *mwfl = targ->mwfl;
  const DictionaryWordList *words = targ->words;

  // Sort pairs by bit_rack
  radix_sort_word_pairs(pairs, count);

  // Count unique bit_racks
  uint32_t num_unique = 0;
  if (count > 0) {
    num_unique = 1;
    for (uint32_t i = 1; i < count; i++) {
      if (!bit_rack_equals(&pairs[i].bit_rack, &pairs[i-1].bit_rack)) {
        num_unique++;
      }
    }
  }

  // Allocate buckets
  mwfl->num_word_buckets = next_power_of_2(num_unique);
  if (mwfl->num_word_buckets < 16) {
    mwfl->num_word_buckets = 16;
  }
  mwfl->word_buckets = malloc_or_die(sizeof(MutableWordMapBucket) * mwfl->num_word_buckets);

  // First pass: count entries per bucket
  uint32_t *bucket_counts = calloc(mwfl->num_word_buckets, sizeof(uint32_t));
  if (bucket_counts == NULL) {
    log_fatal("calloc failed");
  }

  if (count > 0) {
    BitRack *prev = &pairs[0].bit_rack;
    bucket_counts[bit_rack_get_bucket_index(prev, mwfl->num_word_buckets)]++;
    for (uint32_t i = 1; i < count; i++) {
      if (!bit_rack_equals(&pairs[i].bit_rack, prev)) {
        prev = &pairs[i].bit_rack;
        bucket_counts[bit_rack_get_bucket_index(prev, mwfl->num_word_buckets)]++;
      }
    }
  }

  // Allocate bucket entries
  for (uint32_t bucket_idx = 0; bucket_idx < mwfl->num_word_buckets; bucket_idx++) {
    uint32_t c = bucket_counts[bucket_idx];
    mwfl->word_buckets[bucket_idx].num_entries = 0;
    mwfl->word_buckets[bucket_idx].capacity = c > 0 ? c : 1;
    mwfl->word_buckets[bucket_idx].entries =
        malloc_or_die(sizeof(MutableWordMapEntry) * mwfl->word_buckets[bucket_idx].capacity);
  }

  // Second pass: build entries
  if (count > 0) {
    uint32_t run_start = 0;
    for (uint32_t i = 1; i <= count; i++) {
      bool different = (i == count) ||
                       !bit_rack_equals(&pairs[i].bit_rack, &pairs[i-1].bit_rack);
      if (different) {
        uint32_t words_in_run = i - run_start;
        BitRack current_rack = pairs[run_start].bit_rack;
        uint32_t bucket_idx = bit_rack_get_bucket_index(&current_rack, mwfl->num_word_buckets);
        MutableWordMapBucket *bucket = &mwfl->word_buckets[bucket_idx];
        MutableWordMapEntry *entry = &bucket->entries[bucket->num_entries];
        entry->bit_rack = current_rack;
        entry->letters = dictionary_word_list_create_with_capacity((int)words_in_run);

        for (uint32_t j = run_start; j < i; j++) {
          const DictionaryWord *word = dictionary_word_list_get_word(words, (int)pairs[j].word_index);
          dictionary_word_list_add_word(entry->letters,
                                        dictionary_word_get_word(word),
                                        dictionary_word_get_length(word));
        }

        bucket->num_entries++;
        run_start = i;
      }
    }
  }

  free(bucket_counts);
  free(pairs);
  return NULL;
}

// Build word map using sort-and-merge approach with parallel processing
static MutableWordMap *make_mwmp_from_words_sorted(const DictionaryWordList *words) {
  const int total_words = dictionary_word_list_get_count(words);

  // Count words by length
  int num_words_by_length[BOARD_DIM + 1] = {0};
  for (int word_idx = 0; word_idx < total_words; word_idx++) {
    const DictionaryWord *word = dictionary_word_list_get_word(words, word_idx);
    num_words_by_length[dictionary_word_get_length(word)]++;
  }

  // Create pairs array for each length
  WordPair *pairs_by_length[BOARD_DIM + 1] = {NULL};
  uint32_t pair_counts[BOARD_DIM + 1] = {0};

  for (int len = 2; len <= BOARD_DIM; len++) {
    if (num_words_by_length[len] > 0) {
      pairs_by_length[len] = malloc_or_die(sizeof(WordPair) * (uint32_t)num_words_by_length[len]);
    }
  }

  // Generate pairs (sequential - fast)
  for (int word_idx = 0; word_idx < total_words; word_idx++) {
    const DictionaryWord *word = dictionary_word_list_get_word(words, word_idx);
    const uint8_t length = dictionary_word_get_length(word);
    WordPair *pairs = pairs_by_length[length];
    uint32_t idx = pair_counts[length]++;
    pairs[idx].bit_rack = bit_rack_create_from_dictionary_word(word);
    pairs[idx].word_index = (uint32_t)word_idx;
  }

  MutableWordMap *mwmp = malloc_or_die(sizeof(MutableWordMap));

  // Prepare thread arguments
  WordMapThreadArg thread_args[BOARD_DIM + 1];
  pthread_t threads[BOARD_DIM + 1];

  for (int len = 2; len <= BOARD_DIM; len++) {
    if (pair_counts[len] > 0) {
      thread_args[len].words = words;
      thread_args[len].pairs = pairs_by_length[len];
      thread_args[len].pair_count = pair_counts[len];
      thread_args[len].mwfl = &mwmp->maps[len];
      thread_args[len].length = len;
      cpthread_create(&threads[len], build_word_map_for_length, &thread_args[len]);
    } else {
      // Initialize empty map for this length
      mwmp->maps[len].num_word_buckets = 16;
      mwmp->maps[len].word_buckets = malloc_or_die(sizeof(MutableWordMapBucket) * 16);
      for (int i = 0; i < 16; i++) {
        mwmp->maps[len].word_buckets[i].entries = malloc_or_die(sizeof(MutableWordMapEntry));
        mwmp->maps[len].word_buckets[i].num_entries = 0;
        mwmp->maps[len].word_buckets[i].capacity = 1;
      }
    }
  }

  // Wait for all threads
  for (int len = 2; len <= BOARD_DIM; len++) {
    if (pair_counts[len] > 0) {
      cpthread_join(threads[len]);
    }
  }

  return mwmp;
}

// ============================================================================
// Sort-and-merge blank map construction
// ============================================================================

// Build blank map using sort-and-merge approach
static void build_blank_map_sorted(const MutableWordsOfSameLengthMap *mwfl,
                                   MutableBlanksForSameLengthMap *mbfl,
                                   int length) {
  // Count total pairs we'll generate
  uint32_t num_word_entries = 0;
  for (uint32_t bucket_idx = 0; bucket_idx < mwfl->num_word_buckets; bucket_idx++) {
    num_word_entries += mwfl->word_buckets[bucket_idx].num_entries;
  }

  // Estimate: each word has ~length distinct letters
  uint32_t max_pairs = num_word_entries * length;
  BlankPair *pairs = malloc_or_die(sizeof(BlankPair) * max_pairs);
  uint32_t num_pairs = 0;

  // Generate all (bit_rack_with_blank, letter_bit) pairs
  for (uint32_t bucket_idx = 0; bucket_idx < mwfl->num_word_buckets; bucket_idx++) {
    const MutableWordMapBucket *bucket = &mwfl->word_buckets[bucket_idx];
    for (uint32_t entry_idx = 0; entry_idx < bucket->num_entries; entry_idx++) {
      const MutableWordMapEntry *word_entry = &bucket->entries[entry_idx];
      BitRack bit_rack = word_entry->bit_rack;
      uint32_t present = bit_rack_get_letter_mask(&bit_rack) & ~1U;

      while (present) {
        MachineLetter ml = (MachineLetter)bit_ctz32(present);
        present &= present - 1;

        bit_rack_take_letter(&bit_rack, ml);
        bit_rack_add_letter(&bit_rack, BLANK_MACHINE_LETTER);

        pairs[num_pairs].bit_rack = bit_rack;
        pairs[num_pairs].blank_letter_bit = 1U << ml;
        num_pairs++;

        bit_rack_take_letter(&bit_rack, BLANK_MACHINE_LETTER);
        bit_rack_add_letter(&bit_rack, ml);
      }
    }
  }

  // Sort by bit_rack using radix sort
  radix_sort_blank_pairs(pairs, num_pairs);

  // Count unique bit_racks (for sizing)
  uint32_t num_unique = 0;
  if (num_pairs > 0) {
    num_unique = 1;
    for (uint32_t i = 1; i < num_pairs; i++) {
      if (!bit_rack_equals(&pairs[i].bit_rack, &pairs[i-1].bit_rack)) {
        num_unique++;
      }
    }
  }

  // Allocate buckets
  mbfl->num_blank_buckets = next_power_of_2(num_unique);
  if (mbfl->num_blank_buckets < 16) {
    mbfl->num_blank_buckets = 16;
  }
  mbfl->blank_buckets = malloc_or_die(sizeof(MutableBlankMapBucket) * mbfl->num_blank_buckets);

  // First pass: count entries per bucket
  uint32_t *bucket_counts = calloc(mbfl->num_blank_buckets, sizeof(uint32_t));
  if (bucket_counts == NULL) {
    log_fatal("calloc failed");
  }

  if (num_pairs > 0) {
    BitRack *prev = &pairs[0].bit_rack;
    bucket_counts[bit_rack_get_bucket_index(prev, mbfl->num_blank_buckets)]++;
    for (uint32_t i = 1; i < num_pairs; i++) {
      if (!bit_rack_equals(&pairs[i].bit_rack, prev)) {
        prev = &pairs[i].bit_rack;
        bucket_counts[bit_rack_get_bucket_index(prev, mbfl->num_blank_buckets)]++;
      }
    }
  }

  // Allocate bucket entries
  for (uint32_t bucket_idx = 0; bucket_idx < mbfl->num_blank_buckets; bucket_idx++) {
    uint32_t count = bucket_counts[bucket_idx];
    mbfl->blank_buckets[bucket_idx].num_entries = 0;
    mbfl->blank_buckets[bucket_idx].capacity = count > 0 ? count : 1;
    mbfl->blank_buckets[bucket_idx].entries =
        malloc_or_die(sizeof(MutableBlankMapEntry) * mbfl->blank_buckets[bucket_idx].capacity);
  }
  free(bucket_counts);

  // Second pass: merge consecutive entries and insert
  if (num_pairs > 0) {
    BitRack current_rack = pairs[0].bit_rack;
    uint32_t current_bits = pairs[0].blank_letter_bit;

    for (uint32_t i = 1; i < num_pairs; i++) {
      if (bit_rack_equals(&pairs[i].bit_rack, &current_rack)) {
        // Same key - OR in the bits
        current_bits |= pairs[i].blank_letter_bit;
      } else {
        // Different key - emit previous entry
        uint32_t bucket_idx = bit_rack_get_bucket_index(&current_rack, mbfl->num_blank_buckets);
        MutableBlankMapBucket *bucket = &mbfl->blank_buckets[bucket_idx];
        MutableBlankMapEntry *entry = &bucket->entries[bucket->num_entries];
        entry->bit_rack = current_rack;
        entry->blank_letters = current_bits;
        bucket->num_entries++;

        // Start new entry
        current_rack = pairs[i].bit_rack;
        current_bits = pairs[i].blank_letter_bit;
      }
    }

    // Emit final entry
    uint32_t bucket_idx = bit_rack_get_bucket_index(&current_rack, mbfl->num_blank_buckets);
    MutableBlankMapBucket *bucket = &mbfl->blank_buckets[bucket_idx];
    MutableBlankMapEntry *entry = &bucket->entries[bucket->num_entries];
    entry->bit_rack = current_rack;
    entry->blank_letters = current_bits;
    bucket->num_entries++;
  }

  free(pairs);
}

// Thread argument for building blank map for a single length
typedef struct {
  const MutableWordsOfSameLengthMap *mwfl;
  MutableBlanksForSameLengthMap *mbfl;
  int length;
} BlankMapThreadArg;

static void *build_blank_map_thread(void *arg) {
  BlankMapThreadArg *targ = (BlankMapThreadArg *)arg;
  build_blank_map_sorted(targ->mwfl, targ->mbfl, targ->length);
  return NULL;
}

// Build entire blank map using sort-and-merge with parallel processing
static MutableBlankMap *make_mutable_blank_map_sorted(const MutableWordMap *mwmp) {
  MutableBlankMap *mbmp = malloc_or_die(sizeof(MutableBlankMap));

  BlankMapThreadArg thread_args[BOARD_DIM + 1];
  pthread_t threads[BOARD_DIM + 1];

  for (int len = 2; len <= BOARD_DIM; len++) {
    thread_args[len].mwfl = &mwmp->maps[len];
    thread_args[len].mbfl = &mbmp->maps[len];
    thread_args[len].length = len;
    cpthread_create(&threads[len], build_blank_map_thread, &thread_args[len]);
  }

  for (int len = 2; len <= BOARD_DIM; len++) {
    cpthread_join(threads[len]);
  }

  return mbmp;
}

// ============================================================================
// Sort-and-merge double-blank map construction
// ============================================================================

typedef struct MutableDoubleBlankMapEntry {
  BitRack bit_rack;

  // Stored as if they were two-letter words, not yet in any significant order.
  DictionaryWordList *letter_pairs;
} MutableDoubleBlankMapEntry;

typedef struct MutableDoubleBlankMapBucket {
  MutableDoubleBlankMapEntry *entries;
  uint32_t num_entries;
  uint32_t capacity;
} MutableDoubleBlankMapBucket;

typedef struct MutableDoubleBlanksForSameLengthMap {
  MutableDoubleBlankMapBucket *double_blank_buckets;
  uint32_t num_double_blank_buckets;
} MutableDoubleBlanksForSameLengthMap;

typedef struct MutableDoubleBlankMap {
  MutableDoubleBlanksForSameLengthMap maps[BOARD_DIM + 1];
} MutableDoubleBlankMap;

// Build double-blank map using sort-and-merge approach
static void build_double_blank_map_sorted(const MutableWordsOfSameLengthMap *mwfl,
                                          MutableDoubleBlanksForSameLengthMap *mdbfl,
                                          int length) {
  // Count total pairs we'll generate
  uint32_t num_word_entries = 0;
  for (uint32_t bucket_idx = 0; bucket_idx < mwfl->num_word_buckets; bucket_idx++) {
    num_word_entries += mwfl->word_buckets[bucket_idx].num_entries;
  }

  // Estimate: each word has ~length*(length-1)/2 blank pairs
  uint32_t max_pairs = num_word_entries * (uint32_t)length * (uint32_t)length / 2;
  DoubleBlankPair *pairs = malloc_or_die(sizeof(DoubleBlankPair) * max_pairs);
  uint32_t num_pairs = 0;

  // Generate all (bit_rack_with_two_blanks, letter_pair) pairs
  for (uint32_t bucket_idx = 0; bucket_idx < mwfl->num_word_buckets; bucket_idx++) {
    const MutableWordMapBucket *bucket = &mwfl->word_buckets[bucket_idx];
    for (uint32_t entry_idx = 0; entry_idx < bucket->num_entries; entry_idx++) {
      const MutableWordMapEntry *word_entry = &bucket->entries[entry_idx];
      BitRack bit_rack = word_entry->bit_rack;
      uint32_t present1 = bit_rack_get_letter_mask(&bit_rack) & ~1U;

      while (present1) {
        MachineLetter ml1 = (MachineLetter)bit_ctz32(present1);
        present1 &= present1 - 1;

        bit_rack_take_letter(&bit_rack, ml1);
        bit_rack_add_letter(&bit_rack, BLANK_MACHINE_LETTER);

        uint32_t present2 = bit_rack_get_letter_mask(&bit_rack) & ~((1U << ml1) - 1) & ~1U;

        while (present2) {
          MachineLetter ml2 = (MachineLetter)bit_ctz32(present2);
          present2 &= present2 - 1;

          bit_rack_take_letter(&bit_rack, ml2);
          bit_rack_add_letter(&bit_rack, BLANK_MACHINE_LETTER);

          pairs[num_pairs].bit_rack = bit_rack;
          pairs[num_pairs].packed_pair = (uint16_t)ml1 | ((uint16_t)ml2 << 8);
          num_pairs++;

          bit_rack_take_letter(&bit_rack, BLANK_MACHINE_LETTER);
          bit_rack_add_letter(&bit_rack, ml2);
        }

        bit_rack_take_letter(&bit_rack, BLANK_MACHINE_LETTER);
        bit_rack_add_letter(&bit_rack, ml1);
      }
    }
  }

  // Sort by bit_rack then by packed_pair using radix sort
  radix_sort_double_blank_pairs(pairs, num_pairs);

  // Count unique bit_racks and pairs per bit_rack
  uint32_t num_unique = 0;
  if (num_pairs > 0) {
    num_unique = 1;
    for (uint32_t i = 1; i < num_pairs; i++) {
      if (!bit_rack_equals(&pairs[i].bit_rack, &pairs[i-1].bit_rack)) {
        num_unique++;
      }
    }
  }

  // Allocate buckets
  mdbfl->num_double_blank_buckets = next_power_of_2(num_unique);
  if (mdbfl->num_double_blank_buckets < 16) {
    mdbfl->num_double_blank_buckets = 16;
  }
  mdbfl->double_blank_buckets = malloc_or_die(
      sizeof(MutableDoubleBlankMapBucket) * mdbfl->num_double_blank_buckets);

  // First pass: count entries per bucket
  uint32_t *bucket_counts = calloc(mdbfl->num_double_blank_buckets, sizeof(uint32_t));
  if (bucket_counts == NULL) {
    log_fatal("calloc failed");
  }

  if (num_pairs > 0) {
    BitRack *prev = &pairs[0].bit_rack;
    bucket_counts[bit_rack_get_bucket_index(prev, mdbfl->num_double_blank_buckets)]++;
    for (uint32_t i = 1; i < num_pairs; i++) {
      if (!bit_rack_equals(&pairs[i].bit_rack, prev)) {
        prev = &pairs[i].bit_rack;
        bucket_counts[bit_rack_get_bucket_index(prev, mdbfl->num_double_blank_buckets)]++;
      }
    }
  }

  // Allocate bucket entries
  for (uint32_t bucket_idx = 0; bucket_idx < mdbfl->num_double_blank_buckets; bucket_idx++) {
    uint32_t count = bucket_counts[bucket_idx];
    mdbfl->double_blank_buckets[bucket_idx].num_entries = 0;
    mdbfl->double_blank_buckets[bucket_idx].capacity = count > 0 ? count : 1;
    mdbfl->double_blank_buckets[bucket_idx].entries = malloc_or_die(
        sizeof(MutableDoubleBlankMapEntry) * mdbfl->double_blank_buckets[bucket_idx].capacity);
  }
  free(bucket_counts);

  // Second pass: count pairs per unique bit_rack for capacity
  // Then third pass: actually build entries
  if (num_pairs > 0) {
    // Count pairs per entry for preallocating DictionaryWordLists
    uint32_t *pairs_per_entry = malloc_or_die(sizeof(uint32_t) * num_unique);
    uint32_t entry_count = 0;
    uint32_t run_start = 0;

    for (uint32_t i = 1; i <= num_pairs; i++) {
      bool different = (i == num_pairs) ||
                       !bit_rack_equals(&pairs[i].bit_rack, &pairs[i-1].bit_rack);
      if (different) {
        // Count unique pairs in this run
        uint32_t unique_pairs = 1;
        for (uint32_t j = run_start + 1; j < i; j++) {
          if (pairs[j].packed_pair != pairs[j-1].packed_pair) {
            unique_pairs++;
          }
        }
        pairs_per_entry[entry_count++] = unique_pairs;
        run_start = i;
      }
    }

    // Now build entries
    entry_count = 0;
    run_start = 0;

    for (uint32_t i = 1; i <= num_pairs; i++) {
      bool different = (i == num_pairs) ||
                       !bit_rack_equals(&pairs[i].bit_rack, &pairs[i-1].bit_rack);
      if (different) {
        BitRack current_rack = pairs[run_start].bit_rack;
        uint32_t bucket_idx = bit_rack_get_bucket_index(&current_rack,
                                                         mdbfl->num_double_blank_buckets);
        MutableDoubleBlankMapBucket *bucket = &mdbfl->double_blank_buckets[bucket_idx];
        MutableDoubleBlankMapEntry *entry = &bucket->entries[bucket->num_entries];
        entry->bit_rack = current_rack;
        entry->letter_pairs = dictionary_word_list_create_with_capacity(pairs_per_entry[entry_count]);

        // Add unique pairs
        MachineLetter pair_bytes[2];
        uint16_t last_pair = 0xFFFF;  // Invalid value
        for (uint32_t j = run_start; j < i; j++) {
          if (pairs[j].packed_pair != last_pair) {
            pair_bytes[0] = (MachineLetter)(pairs[j].packed_pair & 0xFF);
            pair_bytes[1] = (MachineLetter)(pairs[j].packed_pair >> 8);
            dictionary_word_list_add_word(entry->letter_pairs, pair_bytes, 2);
            last_pair = pairs[j].packed_pair;
          }
        }

        bucket->num_entries++;
        entry_count++;
        run_start = i;
      }
    }

    free(pairs_per_entry);
  }

  free(pairs);
}

// Thread argument for building double-blank map for a single length
typedef struct {
  const MutableWordsOfSameLengthMap *mwfl;
  MutableDoubleBlanksForSameLengthMap *mdbfl;
  int length;
} DoubleBlankMapThreadArg;

static void *build_double_blank_map_thread(void *arg) {
  DoubleBlankMapThreadArg *targ = (DoubleBlankMapThreadArg *)arg;
  build_double_blank_map_sorted(targ->mwfl, targ->mdbfl, targ->length);
  return NULL;
}

// Build entire double-blank map using sort-and-merge with parallel processing
static MutableDoubleBlankMap *make_mutable_double_blank_map_sorted(const MutableWordMap *mwmp) {
  MutableDoubleBlankMap *mdbmp = malloc_or_die(sizeof(MutableDoubleBlankMap));

  DoubleBlankMapThreadArg thread_args[BOARD_DIM + 1];
  pthread_t threads[BOARD_DIM + 1];

  for (int len = 2; len <= BOARD_DIM; len++) {
    thread_args[len].mwfl = &mwmp->maps[len];
    thread_args[len].mdbfl = &mdbmp->maps[len];
    thread_args[len].length = len;
    cpthread_create(&threads[len], build_double_blank_map_thread, &thread_args[len]);
  }

  for (int len = 2; len <= BOARD_DIM; len++) {
    cpthread_join(threads[len]);
  }

  return mdbmp;
}

// Round up to next power of 2
static inline uint32_t next_power_of_2(uint32_t n) {
  if (n == 0) {
    return 1;
  }
  n--;
  n |= n >> 1;
  n |= n >> 2;
  n |= n >> 4;
  n |= n >> 8;
  n |= n >> 16;
  return n + 1;
}

uint32_t get_number_of_words(const MutableWordsOfSameLengthMap *map,
                             const BitRack *bit_rack) {
  const uint32_t bucket_index =
      bit_rack_get_bucket_index(bit_rack, map->num_word_buckets);
  const MutableWordMapBucket *bucket = &map->word_buckets[bucket_index];
  for (uint32_t i = 0; i < bucket->num_entries; i++) {
    const MutableWordMapEntry *entry = &bucket->entries[i];
    if (bit_rack_equals(&entry->bit_rack, bit_rack)) {
      return dictionary_word_list_get_count(entry->letters);
    }
  }
  return 0;
}

BitRack
entry_get_bit_rack_without_blanks(const MutableDoubleBlankMapEntry *entry) {
  BitRack bit_rack = entry->bit_rack;
  bit_rack_set_letter_count(&bit_rack, BLANK_MACHINE_LETTER, 0);
  return bit_rack;
}

uint32_t number_of_double_blank_words(const MutableWordsOfSameLengthMap *wfl,
                                      const MutableDoubleBlankMapEntry *entry) {
  uint32_t num_words = 0;
  const DictionaryWordList *pairs = entry->letter_pairs;
  const int num_pairs = dictionary_word_list_get_count(pairs);
  // Non-const because it is modified to add and take away the blank letters
  BitRack bit_rack = entry_get_bit_rack_without_blanks(entry);
  for (int pair_idx = 0; pair_idx < num_pairs; pair_idx++) {
    const DictionaryWord *pair = dictionary_word_list_get_word(pairs, pair_idx);
    const MachineLetter ml1 = dictionary_word_get_word(pair)[0];
    const MachineLetter ml2 = dictionary_word_get_word(pair)[1];
    bit_rack_add_letter(&bit_rack, ml1);
    bit_rack_add_letter(&bit_rack, ml2);
    num_words += get_number_of_words(wfl, &bit_rack);
    bit_rack_take_letter(&bit_rack, ml2);
    bit_rack_take_letter(&bit_rack, ml1);
  }
  return num_words;
}

uint32_t max_words_for_bucket(const MutableWordsOfSameLengthMap *wfl,
                              const MutableDoubleBlanksForSameLengthMap *dbfl,
                              uint32_t bucket_idx) {
  const MutableDoubleBlankMapBucket *bucket =
      &dbfl->double_blank_buckets[bucket_idx];
  uint32_t max_words = 0;
  for (uint32_t entry_idx = 0; entry_idx < bucket->num_entries; entry_idx++) {
    const MutableDoubleBlankMapEntry *entry = &bucket->entries[entry_idx];
    const uint32_t words = number_of_double_blank_words(wfl, entry);
    if (words > max_words) {
      max_words = words;
    }
  }
  return max_words;
}

uint32_t max_words_for_length(const MutableWordsOfSameLengthMap *wfl,
                              const MutableDoubleBlanksForSameLengthMap *dbfl) {
  uint32_t max_words = 0;
  for (uint32_t bucket_idx = 0; bucket_idx < dbfl->num_double_blank_buckets;
       bucket_idx++) {
    const uint32_t words = max_words_for_bucket(wfl, dbfl, bucket_idx);
    if (words > max_words) {
      max_words = words;
    }
  }
  return max_words;
}

uint32_t
max_word_lookup_result_size(const MutableWordMap *word_map,
                            const MutableDoubleBlankMap *double_blank_map) {
  uint32_t max_size = 0;
  for (int len = 2; len <= BOARD_DIM; len++) {
    const MutableWordsOfSameLengthMap *wfl = &word_map->maps[len];
    const MutableDoubleBlanksForSameLengthMap *dbfl =
        &double_blank_map->maps[len];
    const uint32_t words = max_words_for_length(wfl, dbfl);
    const uint32_t size = len * words;
    if (size > max_size) {
      max_size = size;
    }
  }
  return max_size;
}

int mwfl_get_num_uninlined_words(const MutableWordsOfSameLengthMap *mwfl,
                                 uint32_t word_length) {
  int num_uninlined_words = 0;
  const uint32_t max_inlined = max_inlined_words(word_length);
  for (uint32_t bucket_idx = 0; bucket_idx < mwfl->num_word_buckets;
       bucket_idx++) {
    const MutableWordMapBucket *bucket = &mwfl->word_buckets[bucket_idx];
    for (uint32_t entry_idx = 0; entry_idx < bucket->num_entries; entry_idx++) {
      const MutableWordMapEntry *entry = &bucket->entries[entry_idx];
      const uint32_t num_words = dictionary_word_list_get_count(entry->letters);
      if (num_words > max_inlined) {
        num_uninlined_words += dictionary_word_list_get_count(entry->letters);
      }
    }
  }
  return num_uninlined_words;
}

void write_inlined_word_range(const DictionaryWordList *words,
                              uint8_t bytes[WMP_INLINE_VALUE_BYTES]) {
  memset(bytes, 0, WMP_INLINE_VALUE_BYTES);
  for (int word_idx = 0; word_idx < dictionary_word_list_get_count(words);
       word_idx++) {
    const DictionaryWord *word = dictionary_word_list_get_word(words, word_idx);
    const MachineLetter *word_letters = dictionary_word_get_word(word);
    const int word_length = dictionary_word_get_length(word);
    memcpy(bytes + (ptrdiff_t)(word_idx * word_length), word_letters,
           word_length);
  }
}

void write_uninlined_word_range(uint32_t word_start, uint32_t num_words,
                                WMPEntry *entry) {
  memset(&entry->bucket_or_inline, 0, sizeof(entry->bucket_or_inline));
  entry->word_start = word_start;
  entry->num_words = num_words;
}

void write_letters(const MutableWordMapEntry *entry, uint32_t word_start,
                   uint32_t word_length, MachineLetter *letters) {
  const int num_words = dictionary_word_list_get_count(entry->letters);
  for (int word_idx = 0; word_idx < num_words; word_idx++) {
    const DictionaryWord *word =
        dictionary_word_list_get_word(entry->letters, word_idx);
    const MachineLetter *word_letters = dictionary_word_get_word(word);
    memcpy(letters + (ptrdiff_t)(word_start + word_idx * word_length),
           word_letters, word_length);
  }
}

uint32_t write_word_entries(const MutableWordMapBucket *bucket,
                            uint32_t word_length, WMPEntry *entries,
                            MachineLetter *letters, uint32_t *word_start) {
  for (uint32_t entry_idx = 0; entry_idx < bucket->num_entries; entry_idx++) {
    const MutableWordMapEntry *entry = &bucket->entries[entry_idx];
    wmp_entry_write_bit_rack(&entries[entry_idx], &entry->bit_rack);
    const uint32_t num_words = dictionary_word_list_get_count(entry->letters);
    if (num_words <= max_inlined_words(word_length)) {
      write_inlined_word_range(entry->letters,
                               entries[entry_idx].bucket_or_inline);
      continue;
    }
    write_uninlined_word_range(*word_start, num_words, &entries[entry_idx]);
    write_letters(entry, *word_start, word_length, letters);
    *word_start += num_words * word_length;
  }
  return bucket->num_entries;
}

void fill_wfl_blankless(const MutableWordsOfSameLengthMap *mwfl,
                        uint32_t word_length, WMPForLength *wfl) {
  wfl->num_word_buckets = mwfl->num_word_buckets;
  wfl->word_bucket_starts =
      (uint32_t *)malloc_or_die((wfl->num_word_buckets + 1) * sizeof(uint32_t));
  uint32_t entry_idx = 0;
  for (uint32_t bucket_idx = 0; bucket_idx < mwfl->num_word_buckets;
       bucket_idx++) {
    const MutableWordMapBucket *bucket = &mwfl->word_buckets[bucket_idx];
    wfl->word_bucket_starts[bucket_idx] = entry_idx;
    entry_idx += bucket->num_entries;
  }
  wfl->word_bucket_starts[wfl->num_word_buckets] = entry_idx;

  wfl->num_uninlined_words = mwfl_get_num_uninlined_words(mwfl, word_length);
  wfl->word_letters = (MachineLetter *)malloc_or_die(
      (size_t)wfl->num_uninlined_words * (size_t)word_length);
  wfl->num_word_entries = entry_idx;
  wfl->word_map_entries =
      (WMPEntry *)malloc_or_die(wfl->num_word_entries * sizeof(WMPEntry));
  entry_idx = 0;
  uint32_t word_start = 0;
  for (uint32_t bucket_idx = 0; bucket_idx < wfl->num_word_buckets;
       bucket_idx++) {
    // Writes both the entries and their word letters
    const uint32_t bucket_size = write_word_entries(
        &mwfl->word_buckets[bucket_idx], word_length,
        wfl->word_map_entries + entry_idx, wfl->word_letters, &word_start);
    entry_idx += bucket_size;
  }
}

void write_blank_wmp_entry(const MutableBlankMapEntry *entry,
                           WMPEntry *wmp_entry) {
  memset(wmp_entry->bucket_or_inline, 0, sizeof(wmp_entry->bucket_or_inline));
  wmp_entry->blank_letters = entry->blank_letters;
  wmp_entry_write_bit_rack(wmp_entry, &entry->bit_rack);
}

void fill_wfl_blanks(const MutableBlanksForSameLengthMap *mbfl,
                     WMPForLength *wfl) {
  wfl->num_blank_buckets = mbfl->num_blank_buckets;
  wfl->blank_bucket_starts = (uint32_t *)malloc_or_die(
      (wfl->num_blank_buckets + 1) * sizeof(uint32_t));
  uint32_t overall_entry_idx = 0;
  for (uint32_t bucket_idx = 0; bucket_idx < mbfl->num_blank_buckets;
       bucket_idx++) {
    const MutableBlankMapBucket *bucket = &mbfl->blank_buckets[bucket_idx];
    wfl->blank_bucket_starts[bucket_idx] = overall_entry_idx;
    overall_entry_idx += bucket->num_entries;
  }
  wfl->blank_bucket_starts[wfl->num_blank_buckets] = overall_entry_idx;
  wfl->num_blank_entries = overall_entry_idx;
  wfl->blank_map_entries =
      (WMPEntry *)malloc_or_die(wfl->num_blank_entries * sizeof(WMPEntry));
  overall_entry_idx = 0;
  for (uint32_t bucket_idx = 0; bucket_idx < wfl->num_blank_buckets;
       bucket_idx++) {
    const MutableBlankMapBucket *bucket = &mbfl->blank_buckets[bucket_idx];
    for (uint32_t entry_idx = 0; entry_idx < bucket->num_entries; entry_idx++) {
      const MutableBlankMapEntry *entry = &bucket->entries[entry_idx];
      write_blank_wmp_entry(entry, wfl->blank_map_entries + overall_entry_idx);
      overall_entry_idx++;
    }
  }
}

uint32_t
mdbfl_get_first_blank_letters(const MutableDoubleBlankMapEntry *entry) {
  uint32_t first_blank_letters = 0;
  for (int i = 0; i < dictionary_word_list_get_count(entry->letter_pairs);
       i++) {
    const DictionaryWord *pair =
        dictionary_word_list_get_word(entry->letter_pairs, i);
    const MachineLetter *pair_letters = dictionary_word_get_word(pair);
    first_blank_letters |= 1U << pair_letters[0];
  }
  return first_blank_letters;
}

void write_double_blank_wmp_entry(const MutableDoubleBlankMapEntry *entry,
                                  WMPEntry *wmp_entry) {
  memset(wmp_entry->bucket_or_inline, 0, sizeof(wmp_entry->bucket_or_inline));
  const uint32_t first_blank_letters = mdbfl_get_first_blank_letters(entry);
  wmp_entry->first_blank_letters = first_blank_letters;
  wmp_entry_write_bit_rack(wmp_entry, &entry->bit_rack);
}

void fill_wfl_double_blanks(const MutableDoubleBlanksForSameLengthMap *mdbl,
                            WMPForLength *wfl) {
  wfl->num_double_blank_buckets = mdbl->num_double_blank_buckets;
  wfl->double_blank_bucket_starts = (uint32_t *)malloc_or_die(
      (wfl->num_double_blank_buckets + 1) * sizeof(uint32_t));
  uint32_t overall_entry_idx = 0;
  for (uint32_t bucket_idx = 0; bucket_idx < mdbl->num_double_blank_buckets;
       bucket_idx++) {
    const MutableDoubleBlankMapBucket *bucket =
        &mdbl->double_blank_buckets[bucket_idx];
    wfl->double_blank_bucket_starts[bucket_idx] = overall_entry_idx;
    overall_entry_idx += bucket->num_entries;
  }
  wfl->double_blank_bucket_starts[wfl->num_double_blank_buckets] =
      overall_entry_idx;
  wfl->num_double_blank_entries = overall_entry_idx;
  wfl->double_blank_map_entries = (WMPEntry *)malloc_or_die(
      wfl->num_double_blank_entries * sizeof(WMPEntry));
  overall_entry_idx = 0;
  for (uint32_t bucket_idx = 0; bucket_idx < wfl->num_double_blank_buckets;
       bucket_idx++) {
    const MutableDoubleBlankMapBucket *bucket =
        &mdbl->double_blank_buckets[bucket_idx];
    for (uint32_t entry_idx = 0; entry_idx < bucket->num_entries; entry_idx++) {
      const MutableDoubleBlankMapEntry *entry = &bucket->entries[entry_idx];
      write_double_blank_wmp_entry(entry, wfl->double_blank_map_entries +
                                              overall_entry_idx);
      overall_entry_idx++;
    }
  }
}

void fill_words_of_same_length_map(
    const MutableWordsOfSameLengthMap *mwfl,
    const MutableBlanksForSameLengthMap *mbfl,
    const MutableDoubleBlanksForSameLengthMap *mdbfl, int length,
    WMPForLength *wfl) {
  fill_wfl_blankless(mwfl, length, wfl);
  fill_wfl_blanks(mbfl, wfl);
  fill_wfl_double_blanks(mdbfl, wfl);
}

WMP *make_wmp_from_mutables(const MutableWordMap *word_map,
                            const MutableBlankMap *blank_map,
                            const MutableDoubleBlankMap *double_blank_map) {
  assert(word_map != NULL);
  assert(blank_map != NULL);
  assert(double_blank_map != NULL);
  WMP *wmp = malloc_or_die(sizeof(WMP));
  wmp->name = NULL;
  wmp->version = WMP_VERSION;
  wmp->board_dim = BOARD_DIM;
  wmp->max_word_lookup_bytes =
      max_word_lookup_result_size(word_map, double_blank_map);
  for (int i = 2; i <= BOARD_DIM; i++) {
    fill_words_of_same_length_map(&word_map->maps[i], &blank_map->maps[i],
                                  &double_blank_map->maps[i], i, &wmp->wfls[i]);
  }
  return wmp;
}

void mutable_word_map_bucket_init(MutableWordMapBucket *bucket) {
  bucket->num_entries = 0;
  bucket->capacity = 1;
  bucket->entries = malloc_or_die(sizeof(MutableWordMapEntry));
}

void mutable_word_map_bucket_insert_word(MutableWordMapBucket *bucket,
                                         const BitRack *bit_rack,
                                         const DictionaryWord *word) {
  for (uint32_t entry_idx = 0; entry_idx < bucket->num_entries; entry_idx++) {
    MutableWordMapEntry *entry = &bucket->entries[entry_idx];
    if (bit_rack_equals(&entry->bit_rack, bit_rack)) {
      dictionary_word_list_add_word(entry->letters,
                                    dictionary_word_get_word(word),
                                    dictionary_word_get_length(word));
      return;
    }
  }
  if (bucket->num_entries == bucket->capacity) {
    bucket->capacity *= 2;
    bucket->entries = realloc_or_die(
        bucket->entries, sizeof(MutableWordMapEntry) * bucket->capacity);
  }
  MutableWordMapEntry *entry = &bucket->entries[bucket->num_entries];
  entry->bit_rack = *bit_rack;
  // entry->letters is a DictionaryWordList. Once completed, it will become a
  // contiguous blob of letters like ANESTRIANTSIERNASTIER... in the final WMP
  entry->letters = dictionary_word_list_create_with_capacity(1);
  dictionary_word_list_add_word(entry->letters, dictionary_word_get_word(word),
                                dictionary_word_get_length(word));
  bucket->num_entries++;
}
MutableWordMap *make_mwmp_from_words(const DictionaryWordList *words) {
  MutableWordMap *mwmp = malloc_or_die(sizeof(MutableWordMap));
  int num_words_by_length[BOARD_DIM + 1] = {0};
  for (int word_idx = 0; word_idx < dictionary_word_list_get_count(words);
       word_idx++) {
    const DictionaryWord *word = dictionary_word_list_get_word(words, word_idx);
    num_words_by_length[dictionary_word_get_length(word)]++;
  }
  for (int len = 2; len <= BOARD_DIM; len++) {
    MutableWordsOfSameLengthMap *mwfl = &mwmp->maps[len];
    // Use power-of-2 bucket size for fast modulo via bitwise AND
    mwfl->num_word_buckets = next_power_of_2(num_words_by_length[len]);
    mwfl->word_buckets =
        malloc_or_die(sizeof(MutableWordMapBucket) * mwfl->num_word_buckets);
    for (uint32_t bucket_idx = 0; bucket_idx < mwfl->num_word_buckets;
         bucket_idx++) {
      mutable_word_map_bucket_init(&mwfl->word_buckets[bucket_idx]);
    }
  }
  for (int word_idx = 0; word_idx < dictionary_word_list_get_count(words);
       word_idx++) {
    const DictionaryWord *word = dictionary_word_list_get_word(words, word_idx);
    const uint8_t length = dictionary_word_get_length(word);
    MutableWordsOfSameLengthMap *mwfl = &mwmp->maps[length];
    const BitRack bit_rack = bit_rack_create_from_dictionary_word(word);
    const uint32_t bucket_index =
        bit_rack_get_bucket_index(&bit_rack, mwfl->num_word_buckets);
    MutableWordMapBucket *bucket = &mwfl->word_buckets[bucket_index];
    mutable_word_map_bucket_insert_word(bucket, &bit_rack, word);
  }
  return mwmp;
}

void mutable_blank_map_bucket_init(MutableBlankMapBucket *bucket) {
  bucket->num_entries = 0;
  bucket->capacity = 1;
  bucket->entries = malloc_or_die(sizeof(MutableBlankMapEntry));
}

// Now we store the full bit rack, so just return it directly
BitRack entry_get_full_bit_rack(const MutableWordMapEntry *entry) {
  return entry->bit_rack;
}

void set_blank_map_bit(MutableBlanksForSameLengthMap *mbfl,
                       const BitRack *bit_rack, MachineLetter ml) {
  const uint32_t bucket_index =
      bit_rack_get_bucket_index(bit_rack, mbfl->num_blank_buckets);
  MutableBlankMapBucket *bucket = &mbfl->blank_buckets[bucket_index];
  for (uint32_t entry_idx = 0; entry_idx < bucket->num_entries; entry_idx++) {
    MutableBlankMapEntry *entry = &bucket->entries[entry_idx];
    if (bit_rack_equals(&entry->bit_rack, bit_rack)) {
      entry->blank_letters |= 1U << ml;
      return;
    }
  }
  if (bucket->num_entries == bucket->capacity) {
    bucket->capacity *= 2;
    bucket->entries = realloc_or_die(
        bucket->entries, sizeof(MutableBlankMapEntry) * bucket->capacity);
  }
  MutableBlankMapEntry *entry = &bucket->entries[bucket->num_entries];
  entry->bit_rack = *bit_rack;
  entry->blank_letters = 1U << ml;
  bucket->num_entries++;
}

void insert_blanks_from_word_entry(const MutableWordMapEntry *word_entry,
                                   MutableBlanksForSameLengthMap *mbfl) {
  BitRack bit_rack = entry_get_full_bit_rack(word_entry);
  // NOLINTNEXTLINE(bugprone-too-small-loop-variable)
  for (MachineLetter ml = 1; ml < BIT_RACK_MAX_ALPHABET_SIZE; ml++) {
    if (bit_rack_get_letter(&bit_rack, ml) > 0) {
      bit_rack_take_letter(&bit_rack, ml);
      bit_rack_add_letter(&bit_rack, BLANK_MACHINE_LETTER);
      set_blank_map_bit(mbfl, &bit_rack, ml);
      bit_rack_take_letter(&bit_rack, BLANK_MACHINE_LETTER);
      bit_rack_add_letter(&bit_rack, ml);
    }
  }
}

void insert_blanks_from_word_bucket(const MutableWordMapBucket *mwfl_bucket,
                                    MutableBlanksForSameLengthMap *mbfl) {
  for (uint32_t entry_idx = 0; entry_idx < mwfl_bucket->num_entries;
       entry_idx++) {
    const MutableWordMapEntry *word_entry = &mwfl_bucket->entries[entry_idx];
    insert_blanks_from_word_entry(word_entry, mbfl);
  }
}

void fill_mbfl_from_mwfl(MutableBlanksForSameLengthMap *mbfl,
                         const MutableWordsOfSameLengthMap *mwfl, int length) {
  mbfl->num_blank_buckets = next_power_of_2(mwfl->num_word_buckets * length);
  mbfl->blank_buckets =
      malloc_or_die(sizeof(MutableBlankMapBucket) * mbfl->num_blank_buckets);
  for (uint32_t bucket_idx = 0; bucket_idx < mbfl->num_blank_buckets;
       bucket_idx++) {
    mutable_blank_map_bucket_init(&mbfl->blank_buckets[bucket_idx]);
  }
  for (uint32_t bucket_idx = 0; bucket_idx < mwfl->num_word_buckets;
       bucket_idx++) {
    const MutableWordMapBucket *mwfl_bucket = &mwfl->word_buckets[bucket_idx];
    insert_blanks_from_word_bucket(mwfl_bucket, mbfl);
  }
}

MutableBlankMap *make_mutable_blank_map_from_mwmp(const MutableWordMap *mwmp) {
  MutableBlankMap *mutable_blank_map = malloc_or_die(sizeof(MutableBlankMap));
  for (int len = 2; len <= BOARD_DIM; len++) {
    fill_mbfl_from_mwfl(&mutable_blank_map->maps[len], &mwmp->maps[len], len);
  }
  return mutable_blank_map;
}

void mutable_double_blank_map_bucket_init(MutableDoubleBlankMapBucket *bucket) {
  bucket->num_entries = 0;
  bucket->capacity = 1;
  bucket->entries = malloc_or_die(sizeof(MutableDoubleBlankMapEntry));
}

void mutable_double_blank_map_bucket_insert_pair(
    MutableDoubleBlankMapBucket *bucket, const BitRack *bit_rack,
    const MachineLetter *blanks_as_word) {
  for (uint32_t i = 0; i < bucket->num_entries; i++) {
    MutableDoubleBlankMapEntry *entry = &bucket->entries[i];
    if (bit_rack_equals(&entry->bit_rack, bit_rack)) {
      dictionary_word_list_add_word(entry->letter_pairs, blanks_as_word, 2);
      return;
    }
  }
  if (bucket->num_entries == bucket->capacity) {
    bucket->capacity *= 2;
    bucket->entries = realloc_or_die(
        bucket->entries, sizeof(MutableDoubleBlankMapEntry) * bucket->capacity);
  }
  MutableDoubleBlankMapEntry *entry = &bucket->entries[bucket->num_entries];
  entry->bit_rack = *bit_rack;
  entry->letter_pairs = dictionary_word_list_create_with_capacity(1);
  dictionary_word_list_add_word(entry->letter_pairs, blanks_as_word, 2);
  bucket->num_entries++;
}

void insert_double_blanks_from_word_entry(
    const MutableWordMapEntry *word_entry,
    MutableDoubleBlanksForSameLengthMap *mdbfl) {
  BitRack bit_rack = entry_get_full_bit_rack(word_entry);
  MachineLetter blanks_as_word[2];
  // NOLINTNEXTLINE(bugprone-too-small-loop-variable)
  for (MachineLetter ml1 = 1; ml1 < BIT_RACK_MAX_ALPHABET_SIZE; ml1++) {
    if (bit_rack_get_letter(&bit_rack, ml1) > 0) {
      bit_rack_take_letter(&bit_rack, ml1);
      bit_rack_add_letter(&bit_rack, BLANK_MACHINE_LETTER);
      blanks_as_word[0] = ml1;
      // NOLINTNEXTLINE(bugprone-too-small-loop-variable)
      for (MachineLetter ml2 = ml1; ml2 < BIT_RACK_MAX_ALPHABET_SIZE; ml2++) {
        if (bit_rack_get_letter(&bit_rack, ml2) > 0) {
          bit_rack_take_letter(&bit_rack, ml2);
          bit_rack_add_letter(&bit_rack, BLANK_MACHINE_LETTER);
          blanks_as_word[1] = ml2;
          const uint32_t bucket_index = bit_rack_get_bucket_index(
              &bit_rack, mdbfl->num_double_blank_buckets);
          MutableDoubleBlankMapBucket *bucket =
              &mdbfl->double_blank_buckets[bucket_index];
          mutable_double_blank_map_bucket_insert_pair(bucket, &bit_rack,
                                                      blanks_as_word);
          bit_rack_take_letter(&bit_rack, BLANK_MACHINE_LETTER);
          bit_rack_add_letter(&bit_rack, ml2);
        }
      }
      bit_rack_take_letter(&bit_rack, BLANK_MACHINE_LETTER);
      bit_rack_add_letter(&bit_rack, ml1);
    }
  }
}

void insert_double_blanks_from_word_bucket(
    const MutableWordMapBucket *mwfl_bucket,
    MutableDoubleBlanksForSameLengthMap *mdbfl) {
  for (uint32_t entry_idx = 0; entry_idx < mwfl_bucket->num_entries;
       entry_idx++) {
    const MutableWordMapEntry *word_entry = &mwfl_bucket->entries[entry_idx];
    insert_double_blanks_from_word_entry(word_entry, mdbfl);
  }
}

void fill_mdbfl_from_mwfl(MutableDoubleBlanksForSameLengthMap *mdbfl,
                          const MutableWordsOfSameLengthMap *mwfl, int length) {
  mdbfl->num_double_blank_buckets =
      next_power_of_2(mwfl->num_word_buckets * length);
  mdbfl->double_blank_buckets = malloc_or_die(
      sizeof(MutableDoubleBlankMapBucket) * mdbfl->num_double_blank_buckets);
  for (uint32_t bucket_idx = 0; bucket_idx < mdbfl->num_double_blank_buckets;
       bucket_idx++) {
    mutable_double_blank_map_bucket_init(
        &mdbfl->double_blank_buckets[bucket_idx]);
  }
  for (uint32_t bucket_idx = 0; bucket_idx < mwfl->num_word_buckets;
       bucket_idx++) {
    const MutableWordMapBucket *mwfl_bucket = &mwfl->word_buckets[bucket_idx];
    insert_double_blanks_from_word_bucket(mwfl_bucket, mdbfl);
  }
}

MutableDoubleBlankMap *
make_mutable_double_blank_map_from_mwmp(const MutableWordMap *mwmp) {
  assert(mwmp != NULL);
  MutableDoubleBlankMap *mutable_double_blank_map =
      malloc_or_die(sizeof(MutableDoubleBlankMap));
  for (int len = 2; len <= BOARD_DIM; len++) {
    fill_mdbfl_from_mwfl(&mutable_double_blank_map->maps[len], &mwmp->maps[len],
                         len);
  }
  return mutable_double_blank_map;
}

void mutable_word_map_destroy(MutableWordMap *mwmp) {
  for (int len = 2; len <= BOARD_DIM; len++) {
    MutableWordsOfSameLengthMap *mwfl = &mwmp->maps[len];
    for (uint32_t bucket_idx = 0; bucket_idx < mwfl->num_word_buckets;
         bucket_idx++) {
      MutableWordMapBucket *bucket = &mwfl->word_buckets[bucket_idx];
      for (uint32_t entry_idx = 0; entry_idx < bucket->num_entries;
           entry_idx++) {
        MutableWordMapEntry *entry = &bucket->entries[entry_idx];
        dictionary_word_list_destroy(entry->letters);
      }
      free(bucket->entries);
    }
    free(mwfl->word_buckets);
  }
  free(mwmp);
}

void mutable_blank_map_destroy(MutableBlankMap *mbmp) {
  for (int len = 2; len <= BOARD_DIM; len++) {
    MutableBlanksForSameLengthMap *mbfl = &mbmp->maps[len];
    for (uint32_t bucket_idx = 0; bucket_idx < mbfl->num_blank_buckets;
         bucket_idx++) {
      MutableBlankMapBucket *bucket = &mbfl->blank_buckets[bucket_idx];
      free(bucket->entries);
    }
    free(mbfl->blank_buckets);
  }
  free(mbmp);
}

void mutable_double_blank_map_destroy(MutableDoubleBlankMap *mdbmp) {
  for (int len = 2; len <= BOARD_DIM; len++) {
    MutableDoubleBlanksForSameLengthMap *mdbfl = &mdbmp->maps[len];
    for (uint32_t bucket_idx = 0; bucket_idx < mdbfl->num_double_blank_buckets;
         bucket_idx++) {
      MutableDoubleBlankMapBucket *bucket =
          &mdbfl->double_blank_buckets[bucket_idx];
      for (uint32_t entry_idx = 0; entry_idx < bucket->num_entries;
           entry_idx++) {
        MutableDoubleBlankMapEntry *entry = &bucket->entries[entry_idx];
        dictionary_word_list_destroy(entry->letter_pairs);
      }
      free(bucket->entries);
    }
    free(mdbfl->double_blank_buckets);
  }
  free(mdbmp);
}

uint32_t mwfl_get_num_entries(const MutableWordsOfSameLengthMap *mwfl) {
  uint32_t num_sets = 0;
  for (uint32_t i = 0; i < mwfl->num_word_buckets; i++) {
    const MutableWordMapBucket *bucket = &mwfl->word_buckets[i];
    num_sets += bucket->num_entries;
  }
  return num_sets;
}

void reinsert_word_entry(const MutableWordMapEntry *entry,
                         MutableWordsOfSameLengthMap *new_mwfl) {
  const int num_words = dictionary_word_list_get_count(entry->letters);
  for (int word_idx = 0; word_idx < num_words; word_idx++) {
    const DictionaryWord *word =
        dictionary_word_list_get_word(entry->letters, word_idx);
    const BitRack bit_rack = bit_rack_create_from_dictionary_word(word);
    const uint32_t bucket_index =
        bit_rack_get_bucket_index(&bit_rack, new_mwfl->num_word_buckets);
    MutableWordMapBucket *bucket = &new_mwfl->word_buckets[bucket_index];
    mutable_word_map_bucket_insert_word(bucket, &bit_rack, word);
  }
}

void reinsert_entries_from_word_bucket(const MutableWordMapBucket *bucket,
                                       MutableWordsOfSameLengthMap *new_mwfl) {
  for (uint32_t entry_idx = 0; entry_idx < bucket->num_entries; entry_idx++) {
    const MutableWordMapEntry *entry = &bucket->entries[entry_idx];
    reinsert_word_entry(entry, new_mwfl);
  }
}

void fill_resized_mwfl(const MutableWordsOfSameLengthMap *mwfl,
                       MutableWordsOfSameLengthMap *new_mwfl) {
  const uint32_t num_entries = mwfl_get_num_entries(mwfl);
  new_mwfl->num_word_buckets = next_power_of_2(num_entries);
  new_mwfl->word_buckets =
      malloc_or_die(sizeof(MutableWordMapBucket) * new_mwfl->num_word_buckets);
  for (uint32_t bucket_idx = 0; bucket_idx < new_mwfl->num_word_buckets;
       bucket_idx++) {
    mutable_word_map_bucket_init(&new_mwfl->word_buckets[bucket_idx]);
  }
  for (uint32_t bucket_idx = 0; bucket_idx < mwfl->num_word_buckets;
       bucket_idx++) {
    const MutableWordMapBucket *bucket = &mwfl->word_buckets[bucket_idx];
    reinsert_entries_from_word_bucket(bucket, new_mwfl);
  }
}

MutableWordMap *resize_mutable_word_map(const MutableWordMap *mwmp) {
  MutableWordMap *resized_mwmp = malloc_or_die(sizeof(MutableWordMap));
  for (int len = 2; len <= BOARD_DIM; len++) {
    const MutableWordsOfSameLengthMap *mwfl = &mwmp->maps[len];
    MutableWordsOfSameLengthMap *new_mwfl = &resized_mwmp->maps[len];
    fill_resized_mwfl(mwfl, new_mwfl);
  }
  return resized_mwmp;
}

void set_blank_map_bits(const BitRack *bit_rack, uint32_t blank_letters,
                        MutableBlanksForSameLengthMap *new_mbfl) {
  const uint32_t bucket_idx =
      bit_rack_get_bucket_index(bit_rack, new_mbfl->num_blank_buckets);
  MutableBlankMapBucket *bucket = &new_mbfl->blank_buckets[bucket_idx];
  // We're always adding a new k/v pair rather than modifying.
  if (bucket->num_entries == bucket->capacity) {
    bucket->capacity *= 2;
    bucket->entries = realloc_or_die(
        bucket->entries, sizeof(MutableBlankMapEntry) * bucket->capacity);
  }
  MutableBlankMapEntry *entry = &bucket->entries[bucket->num_entries];
  entry->bit_rack = *bit_rack;
  entry->blank_letters = blank_letters;
  bucket->num_entries++;
}

void reinsert_blank_entry(const MutableBlankMapEntry *entry,
                          MutableBlanksForSameLengthMap *new_mbfl) {
  set_blank_map_bits(&entry->bit_rack, entry->blank_letters, new_mbfl);
}

void reinsert_entries_from_blank_bucket(
    const MutableBlankMapBucket *bucket,
    MutableBlanksForSameLengthMap *new_mbfl) {
  for (uint32_t entry_idx = 0; entry_idx < bucket->num_entries; entry_idx++) {
    const MutableBlankMapEntry *entry = &bucket->entries[entry_idx];
    reinsert_blank_entry(entry, new_mbfl);
  }
}

uint32_t mbfl_get_num_entries(const MutableBlanksForSameLengthMap *mbfl) {
  uint32_t num_entries = 0;
  for (uint32_t bucket_idx = 0; bucket_idx < mbfl->num_blank_buckets;
       bucket_idx++) {
    const MutableBlankMapBucket *bucket = &mbfl->blank_buckets[bucket_idx];
    num_entries += bucket->num_entries;
  }
  return num_entries;
}

void fill_resized_mbfl(const MutableBlanksForSameLengthMap *mbfl,
                       MutableBlanksForSameLengthMap *new_mbfl) {
  const uint32_t num_entries = mbfl_get_num_entries(mbfl);
  new_mbfl->num_blank_buckets = next_power_of_2(num_entries);
  new_mbfl->blank_buckets = malloc_or_die(sizeof(MutableBlankMapBucket) *
                                          new_mbfl->num_blank_buckets);
  for (uint32_t bucket_idx = 0; bucket_idx < new_mbfl->num_blank_buckets;
       bucket_idx++) {
    mutable_blank_map_bucket_init(&new_mbfl->blank_buckets[bucket_idx]);
  }
  for (uint32_t bucket_idx = 0; bucket_idx < mbfl->num_blank_buckets;
       bucket_idx++) {
    const MutableBlankMapBucket *bucket = &mbfl->blank_buckets[bucket_idx];
    reinsert_entries_from_blank_bucket(bucket, new_mbfl);
  }
}

MutableBlankMap *resize_mutable_blank_map(const MutableBlankMap *mbmp) {
  MutableBlankMap *resized_mbmp = malloc_or_die(sizeof(MutableBlankMap));
  for (int len = 2; len <= BOARD_DIM; len++) {
    const MutableBlanksForSameLengthMap *mbfl = &mbmp->maps[len];
    MutableBlanksForSameLengthMap *new_mbfl = &resized_mbmp->maps[len];
    fill_resized_mbfl(mbfl, new_mbfl);
  }
  return resized_mbmp;
}

uint32_t
mdbfl_get_num_entries(const MutableDoubleBlanksForSameLengthMap *mdbfl) {
  uint32_t num_entries = 0;
  for (uint32_t bucket_idx = 0; bucket_idx < mdbfl->num_double_blank_buckets;
       bucket_idx++) {
    const MutableDoubleBlankMapBucket *bucket =
        &mdbfl->double_blank_buckets[bucket_idx];
    num_entries += bucket->num_entries;
  }
  return num_entries;
}

void reinsert_double_blank_entry(
    const MutableDoubleBlankMapEntry *entry,
    MutableDoubleBlanksForSameLengthMap *new_mdbfl) {
  for (int pair_idx = 0;
       pair_idx < dictionary_word_list_get_count(entry->letter_pairs);
       pair_idx++) {
    const DictionaryWord *pair =
        dictionary_word_list_get_word(entry->letter_pairs, pair_idx);
    const uint8_t *pair_letters = dictionary_word_get_word(pair);
    const uint32_t new_bucket_idx = bit_rack_get_bucket_index(
        &entry->bit_rack, new_mdbfl->num_double_blank_buckets);
    MutableDoubleBlankMapBucket *bucket =
        &new_mdbfl->double_blank_buckets[new_bucket_idx];
    mutable_double_blank_map_bucket_insert_pair(bucket, &entry->bit_rack,
                                                pair_letters);
  }
}

void reinsert_entries_from_double_blank_bucket(
    const MutableDoubleBlankMapBucket *bucket,
    MutableDoubleBlanksForSameLengthMap *new_mdbfl) {
  for (uint32_t entry_idx = 0; entry_idx < bucket->num_entries; entry_idx++) {
    const MutableDoubleBlankMapEntry *entry = &bucket->entries[entry_idx];
    reinsert_double_blank_entry(entry, new_mdbfl);
  }
}

void fill_resized_mdbfl(const MutableDoubleBlanksForSameLengthMap *mdbfl,
                        MutableDoubleBlanksForSameLengthMap *new_mdbfl) {
  const uint32_t num_entries = mdbfl_get_num_entries(mdbfl);
  new_mdbfl->num_double_blank_buckets = next_power_of_2(num_entries);
  new_mdbfl->double_blank_buckets =
      malloc_or_die(sizeof(MutableDoubleBlankMapBucket) *
                    new_mdbfl->num_double_blank_buckets);
  for (uint32_t bucket_idx = 0;
       bucket_idx < new_mdbfl->num_double_blank_buckets; bucket_idx++) {
    mutable_double_blank_map_bucket_init(
        &new_mdbfl->double_blank_buckets[bucket_idx]);
  }
  for (uint32_t bucket_idx = 0; bucket_idx < mdbfl->num_double_blank_buckets;
       bucket_idx++) {
    const MutableDoubleBlankMapBucket *bucket =
        &mdbfl->double_blank_buckets[bucket_idx];
    reinsert_entries_from_double_blank_bucket(bucket, new_mdbfl);
  }
}

MutableDoubleBlankMap *
resize_mutable_double_blank_map(const MutableDoubleBlankMap *mdbmp) {
  MutableDoubleBlankMap *resized_mdbmp =
      malloc_or_die(sizeof(MutableDoubleBlankMap));
  for (int len = 2; len <= BOARD_DIM; len++) {
    const MutableDoubleBlanksForSameLengthMap *mdbfl = &mdbmp->maps[len];
    MutableDoubleBlanksForSameLengthMap *new_mdbfl = &resized_mdbmp->maps[len];
    fill_resized_mdbfl(mdbfl, new_mdbfl);
  }
  return resized_mdbmp;
}

WMP *make_wmp_from_words(const DictionaryWordList *words,
                         const LetterDistribution *ld) {
  if (ld->distribution[BLANK_MACHINE_LETTER] > 2) {
    log_fatal("cannot create WMP with more than 2 blanks");
    return NULL;
  }
  // Build word map using sort-and-merge (already properly sized)
  MutableWordMap *mwmp = make_mwmp_from_words_sorted(words);

  // Build blank map and double-blank map using sort-and-merge (fast)
  MutableBlankMap *mbmp = make_mutable_blank_map_sorted(mwmp);
  MutableDoubleBlankMap *mdbmp = make_mutable_double_blank_map_sorted(mwmp);

  WMP *wmp = make_wmp_from_mutables(mwmp, mbmp, mdbmp);
  mutable_word_map_destroy(mwmp);
  mutable_blank_map_destroy(mbmp);
  mutable_double_blank_map_destroy(mdbmp);
  return wmp;
}