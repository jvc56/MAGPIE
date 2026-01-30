#include "wmp_maker.h"

#include "../compat/cpthread.h"
#include "../compat/memory_info.h"
#include "../def/bit_rack_defs.h"
#include "../def/board_defs.h"
#include "../def/cpthread_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/wmp_defs.h"
#include "../ent/bit_rack.h"
#include "../ent/dictionary_word.h"
#include "../ent/kwg.h"
#include "../ent/letter_distribution.h"
#include "../ent/wmp.h"
#include "../util/io_util.h"
#include "kwg_maker.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Utility functions
// ============================================================================

// Minimum number of hash buckets for WMP tables
enum { MIN_BUCKETS = 16 };

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
    if (low & 0xF) {
      mask |= 1U << i;
    }
    low >>= 4;
  }
  for (int i = 0; i < 16; i++) {
    if (high & 0xF) {
      mask |= 1U << (i + 16);
    }
    high >>= 4;
  }
  return mask;
}

// Software prefetch hint
#if defined(__GNUC__) || defined(__clang__)
#define PREFETCH(addr) __builtin_prefetch(addr, 0, 1)
#else
#define PREFETCH(addr) (void)(addr)
#endif

// Prefetch distance (number of elements ahead to prefetch)
enum { PREFETCH_DISTANCE = 16 };

// Number of radix passes needed based on alphabet size
// Each letter uses 4 bits, so bytes_needed = ceil(alphabet_size * 4 / 8)
static inline int radix_passes_for_alphabet(int alphabet_size) {
  // alphabet_size letters * 4 bits per letter / 8 bits per byte, rounded up
  return (alphabet_size * BIT_RACK_BITS_PER_LETTER + 7) / 8;
}

// ============================================================================
// Sort pair structures - use a union for buffer reuse
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
  uint16_t packed_pair; // ml1 | (ml2 << 8)
} DoubleBlankPair;

// All pair types have the same size (24 bytes with alignment)
// This allows buffer reuse across phases
static_assert(sizeof(WordPair) == sizeof(BlankPair), "Pair sizes must match");
static_assert(sizeof(WordPair) >= sizeof(DoubleBlankPair),
              "WordPair must be >= DoubleBlankPair");

// ============================================================================
// Radix sort implementations with prefetching
//
// Uses LSD (Least-Significant-Digit) radix sort for 128-bit BitRack keys.
// The number of passes is alphabet-aware: English (27 letters × 4 bits = 108
// bits) needs 14 passes instead of 16, reducing work by ~12%.
// ============================================================================

static void radix_pass_word_pairs(WordPair *src, WordPair *dst, uint32_t count,
                                  int byte_idx) {
  uint32_t counts[256] = {0};

  // Count phase with prefetching
  for (uint32_t i = 0; i < count; i++) {
    if (i + PREFETCH_DISTANCE < count) {
      PREFETCH(&src[i + PREFETCH_DISTANCE]);
    }
    uint8_t byte;
    if (byte_idx < 8) {
      byte = (bit_rack_get_low_64(&src[i].bit_rack) >> (byte_idx * 8)) & 0xFF;
    } else {
      byte = (bit_rack_get_high_64(&src[i].bit_rack) >> ((byte_idx - 8) * 8)) &
             0xFF;
    }
    counts[byte]++;
  }

  // Prefix sum
  uint32_t total = 0;
  for (int b = 0; b < 256; b++) {
    uint32_t c = counts[b];
    counts[b] = total;
    total += c;
  }

  // Scatter phase with prefetching
  for (uint32_t i = 0; i < count; i++) {
    if (i + PREFETCH_DISTANCE < count) {
      PREFETCH(&src[i + PREFETCH_DISTANCE]);
    }
    uint8_t byte;
    if (byte_idx < 8) {
      byte = (bit_rack_get_low_64(&src[i].bit_rack) >> (byte_idx * 8)) & 0xFF;
    } else {
      byte = (bit_rack_get_high_64(&src[i].bit_rack) >> ((byte_idx - 8) * 8)) &
             0xFF;
    }
    dst[counts[byte]++] = src[i];
  }
}

static void radix_sort_word_pairs(WordPair *pairs, WordPair *temp,
                                  uint32_t count, int num_passes) {
  if (count <= 1) {
    return;
  }
  for (int pass = 0; pass < num_passes; pass++) {
    if (pass % 2 == 0) {
      radix_pass_word_pairs(pairs, temp, count, pass);
    } else {
      radix_pass_word_pairs(temp, pairs, count, pass);
    }
  }
  // If odd number of passes, result is in temp - copy back
  if (num_passes % 2 == 1) {
    memcpy(pairs, temp, count * sizeof(WordPair));
  }
}

static void radix_pass_blank_pairs(BlankPair *src, BlankPair *dst,
                                   uint32_t count, int byte_idx) {
  uint32_t counts[256] = {0};

  for (uint32_t i = 0; i < count; i++) {
    if (i + PREFETCH_DISTANCE < count) {
      PREFETCH(&src[i + PREFETCH_DISTANCE]);
    }
    uint8_t byte;
    if (byte_idx < 8) {
      byte = (bit_rack_get_low_64(&src[i].bit_rack) >> (byte_idx * 8)) & 0xFF;
    } else {
      byte = (bit_rack_get_high_64(&src[i].bit_rack) >> ((byte_idx - 8) * 8)) &
             0xFF;
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
    if (i + PREFETCH_DISTANCE < count) {
      PREFETCH(&src[i + PREFETCH_DISTANCE]);
    }
    uint8_t byte;
    if (byte_idx < 8) {
      byte = (bit_rack_get_low_64(&src[i].bit_rack) >> (byte_idx * 8)) & 0xFF;
    } else {
      byte = (bit_rack_get_high_64(&src[i].bit_rack) >> ((byte_idx - 8) * 8)) &
             0xFF;
    }
    dst[counts[byte]++] = src[i];
  }
}

static void radix_sort_blank_pairs(BlankPair *pairs, BlankPair *temp,
                                   uint32_t count, int num_passes) {
  if (count <= 1) {
    return;
  }
  for (int pass = 0; pass < num_passes; pass++) {
    if (pass % 2 == 0) {
      radix_pass_blank_pairs(pairs, temp, count, pass);
    } else {
      radix_pass_blank_pairs(temp, pairs, count, pass);
    }
  }
  // If odd number of passes, result is in temp - copy back
  if (num_passes % 2 == 1) {
    memcpy(pairs, temp, count * sizeof(BlankPair));
  }
}

static void radix_pass_double_blank_pairs(DoubleBlankPair *src,
                                          DoubleBlankPair *dst, uint32_t count,
                                          int byte_idx) {
  uint32_t counts[256] = {0};

  for (uint32_t i = 0; i < count; i++) {
    if (i + PREFETCH_DISTANCE < count) {
      PREFETCH(&src[i + PREFETCH_DISTANCE]);
    }
    uint8_t byte;
    if (byte_idx < 2) {
      byte = (src[i].packed_pair >> (byte_idx * 8)) & 0xFF;
    } else if (byte_idx < 10) {
      byte = (bit_rack_get_low_64(&src[i].bit_rack) >> ((byte_idx - 2) * 8)) &
             0xFF;
    } else {
      byte = (bit_rack_get_high_64(&src[i].bit_rack) >> ((byte_idx - 10) * 8)) &
             0xFF;
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
    if (i + PREFETCH_DISTANCE < count) {
      PREFETCH(&src[i + PREFETCH_DISTANCE]);
    }
    uint8_t byte;
    if (byte_idx < 2) {
      byte = (src[i].packed_pair >> (byte_idx * 8)) & 0xFF;
    } else if (byte_idx < 10) {
      byte = (bit_rack_get_low_64(&src[i].bit_rack) >> ((byte_idx - 2) * 8)) &
             0xFF;
    } else {
      byte = (bit_rack_get_high_64(&src[i].bit_rack) >> ((byte_idx - 10) * 8)) &
             0xFF;
    }
    dst[counts[byte]++] = src[i];
  }
}

static void radix_sort_double_blank_pairs(DoubleBlankPair *pairs,
                                          DoubleBlankPair *temp, uint32_t count,
                                          int num_bitrack_passes) {
  if (count <= 1) {
    return;
  }
  // 2 passes for packed_pair + num_bitrack_passes for BitRack
  int total_passes = 2 + num_bitrack_passes;
  for (int pass = 0; pass < total_passes; pass++) {
    if (pass % 2 == 0) {
      radix_pass_double_blank_pairs(pairs, temp, count, pass);
    } else {
      radix_pass_double_blank_pairs(temp, pairs, count, pass);
    }
  }
  // If odd number of passes, result is in temp - copy back
  if (total_passes % 2 == 1) {
    memcpy(pairs, temp, count * sizeof(DoubleBlankPair));
  }
}

// ============================================================================
// Shared scratch buffer for each word length
//
// Each word length gets its own scratch buffers, allowing all lengths to be
// processed in parallel. The buffers are reused across phases 1-3 to avoid
// repeated allocations. WordPair, BlankPair, and DoubleBlankPair are all the
// same size (verified by static_assert), enabling buffer reuse.
// ============================================================================

typedef struct {
  // Pre-allocated buffers that can be reused across phases
  void *scratch1;      // Used as pairs array
  void *scratch2;      // Used as temp array for sorting
  size_t scratch_size; // Size in bytes of each scratch buffer

  // Bucket counts buffer (reused across phases)
  uint32_t *bucket_counts;
  uint32_t bucket_counts_size;

  // Unique racks extracted from word phase (shared between blank and
  // double-blank)
  BitRack *unique_racks;
  uint32_t num_unique_racks;

  // Number of radix passes needed (based on alphabet size)
  int radix_passes;
} LengthScratchBuffers;

// ============================================================================
// Thread limiting semaphore
//
// Counting semaphore that limits concurrent threads to the user's -threads N
// setting. Threads acquire before starting work and release when done. This
// allows the main thread to launch new work as soon as any thread finishes,
// providing better load balancing than a fixed thread pool.
// ============================================================================

typedef struct {
  cpthread_mutex_t mutex;
  cpthread_cond_t cond;
  int count;
  int max_count;
} ThreadSemaphore;

static void thread_sem_init(ThreadSemaphore *sem, int max_count) {
  cpthread_mutex_init(&sem->mutex);
  cpthread_cond_init(&sem->cond);
  sem->count = max_count;
  sem->max_count = max_count;
}

static void thread_sem_acquire(ThreadSemaphore *sem) {
  cpthread_mutex_lock(&sem->mutex);
  while (sem->count == 0) {
    cpthread_cond_wait(&sem->cond, &sem->mutex);
  }
  sem->count--;
  cpthread_mutex_unlock(&sem->mutex);
}

static void thread_sem_release(ThreadSemaphore *sem) {
  cpthread_mutex_lock(&sem->mutex);
  sem->count++;
  cpthread_cond_signal(&sem->cond);
  cpthread_mutex_unlock(&sem->mutex);
}

// ============================================================================
// Phase 1: Build word entries and extract unique racks
// ============================================================================

typedef struct {
  const DictionaryWordList *words;
  WMPForLength *wfl;
  WordPair *pairs;
  WordPair *temp;
  LengthScratchBuffers *scratch;
  ThreadSemaphore *sem; // NULL if running single-threaded
  int length;
  uint32_t pair_count;
} WordBuildArg;

static void *build_words_and_extract_racks(void *arg) {
  WordBuildArg *a = (WordBuildArg *)arg;
  WordPair *pairs = a->pairs;
  WordPair *temp = a->temp;
  uint32_t count = a->pair_count;
  WMPForLength *wfl = a->wfl;
  const DictionaryWordList *words = a->words;
  const int word_length = a->length;
  LengthScratchBuffers *scratch = a->scratch;

  // Sort pairs
  radix_sort_word_pairs(pairs, temp, count, scratch->radix_passes);

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

  // Extract unique racks for blank/double-blank phases (stored in scratch)
  scratch->num_unique_racks = num_unique;
  scratch->unique_racks =
      malloc_or_die(sizeof(BitRack) * (num_unique > 0 ? num_unique : 1));
  if (count > 0) {
    uint32_t rack_idx = 0;
    scratch->unique_racks[rack_idx++] = pairs[0].bit_rack;
    for (uint32_t i = 1; i < count; i++) {
      if (!bit_rack_equals(&pairs[i].bit_rack, &pairs[i - 1].bit_rack)) {
        scratch->unique_racks[rack_idx++] = pairs[i].bit_rack;
      }
    }
  }

  // Build word entries
  wfl->num_word_buckets = next_power_of_2(num_unique);
  if (wfl->num_word_buckets < MIN_BUCKETS) {
    wfl->num_word_buckets = MIN_BUCKETS;
  }

  // Resize bucket_counts if needed
  if (wfl->num_word_buckets > scratch->bucket_counts_size) {
    free(scratch->bucket_counts);
    scratch->bucket_counts = calloc(wfl->num_word_buckets, sizeof(uint32_t));
    scratch->bucket_counts_size = wfl->num_word_buckets;
  } else {
    memset(scratch->bucket_counts, 0, wfl->num_word_buckets * sizeof(uint32_t));
  }
  uint32_t *bucket_counts = scratch->bucket_counts;

  const uint32_t max_inline = max_inlined_words(word_length);
  uint32_t num_uninlined_letters = 0;

  if (count > 0) {
    uint32_t run_start = 0;
    for (uint32_t i = 1; i <= count; i++) {
      bool is_end = (i == count) || !bit_rack_equals(&pairs[i].bit_rack,
                                                     &pairs[i - 1].bit_rack);
      if (is_end) {
        uint32_t words_in_run = i - run_start;
        bucket_counts[bit_rack_get_bucket_index(&pairs[run_start].bit_rack,
                                                wfl->num_word_buckets)]++;
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
  wfl->word_letters =
      malloc_or_die(num_uninlined_letters > 0 ? num_uninlined_letters : 1);
  wfl->word_bucket_starts =
      malloc_or_die((wfl->num_word_buckets + 1) * sizeof(uint32_t));

  uint32_t offset = 0;
  for (uint32_t b = 0; b < wfl->num_word_buckets; b++) {
    wfl->word_bucket_starts[b] = offset;
    offset += bucket_counts[b];
  }
  wfl->word_bucket_starts[wfl->num_word_buckets] = offset;

  memset(bucket_counts, 0, wfl->num_word_buckets * sizeof(uint32_t));

  if (count > 0) {
    uint32_t letter_offset = 0;
    uint32_t run_start = 0;
    for (uint32_t i = 1; i <= count; i++) {
      bool is_end = (i == count) || !bit_rack_equals(&pairs[i].bit_rack,
                                                     &pairs[i - 1].bit_rack);
      if (is_end) {
        uint32_t words_in_run = i - run_start;
        BitRack rack = pairs[run_start].bit_rack;
        uint32_t bucket_idx =
            bit_rack_get_bucket_index(&rack, wfl->num_word_buckets);
        uint32_t entry_idx =
            wfl->word_bucket_starts[bucket_idx] + bucket_counts[bucket_idx]++;

        WMPEntry *entry = &wfl->word_map_entries[entry_idx];
        wmp_entry_write_bit_rack(entry, &rack);

        if (words_in_run <= max_inline) {
          memset(entry->bucket_or_inline, 0, WMP_INLINE_VALUE_BYTES);
          for (uint32_t j = 0; j < words_in_run; j++) {
            const DictionaryWord *word = dictionary_word_list_get_word(
                words, (int)pairs[run_start + j].word_index);
            memcpy(entry->bucket_or_inline + (size_t)j * word_length,
                   dictionary_word_get_word(word), word_length);
          }
        } else {
          memset(entry->bucket_or_inline, 0, WMP_INLINE_VALUE_BYTES);
          entry->word_start = letter_offset;
          entry->num_words = words_in_run;
          for (uint32_t j = 0; j < words_in_run; j++) {
            const DictionaryWord *word = dictionary_word_list_get_word(
                words, (int)pairs[run_start + j].word_index);
            memcpy(wfl->word_letters + letter_offset,
                   dictionary_word_get_word(word), word_length);
            letter_offset += word_length;
          }
        }
        run_start = i;
      }
    }
  }

  if (a->sem) {
    thread_sem_release(a->sem);
  }
  return NULL;
}

// ============================================================================
// Phase 2: Build blank entries using scratch buffers
// ============================================================================

typedef struct {
  LengthScratchBuffers *scratch;
  WMPForLength *wfl;
  int length;
  ThreadSemaphore *sem;
} BlankBuildArg;

static void *build_blank_entries_direct(void *arg) {
  BlankBuildArg *a = (BlankBuildArg *)arg;
  LengthScratchBuffers *scratch = a->scratch;
  const BitRack *unique_racks = scratch->unique_racks;
  uint32_t num_racks = scratch->num_unique_racks;
  WMPForLength *wfl = a->wfl;

  // Estimate max pairs and check scratch buffer size
  size_t max_pairs = (size_t)num_racks * (size_t)a->length;
  size_t needed_size = sizeof(BlankPair) * max_pairs;

  // Reuse scratch buffers if large enough, otherwise reallocate
  if (needed_size > scratch->scratch_size) {
    free(scratch->scratch1);
    free(scratch->scratch2);
    scratch->scratch1 = malloc_or_die(needed_size);
    scratch->scratch2 = malloc_or_die(needed_size);
    scratch->scratch_size = needed_size;
  }

  BlankPair *pairs = (BlankPair *)scratch->scratch1;
  BlankPair *temp = (BlankPair *)scratch->scratch2;
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
  radix_sort_blank_pairs(pairs, temp, num_pairs, scratch->radix_passes);

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
  wfl->num_blank_buckets = next_power_of_2(num_unique);
  if (wfl->num_blank_buckets < MIN_BUCKETS) {
    wfl->num_blank_buckets = MIN_BUCKETS;
  }

  // Resize bucket_counts if needed
  if (wfl->num_blank_buckets > scratch->bucket_counts_size) {
    free(scratch->bucket_counts);
    scratch->bucket_counts = calloc(wfl->num_blank_buckets, sizeof(uint32_t));
    scratch->bucket_counts_size = wfl->num_blank_buckets;
  } else {
    memset(scratch->bucket_counts, 0,
           wfl->num_blank_buckets * sizeof(uint32_t));
  }
  uint32_t *bucket_counts = scratch->bucket_counts;

  if (num_pairs > 0) {
    const BitRack *prev = &pairs[0].bit_rack;
    bucket_counts[bit_rack_get_bucket_index(prev, wfl->num_blank_buckets)]++;
    for (uint32_t i = 1; i < num_pairs; i++) {
      if (!bit_rack_equals(&pairs[i].bit_rack, prev)) {
        prev = &pairs[i].bit_rack;
        bucket_counts[bit_rack_get_bucket_index(prev,
                                                wfl->num_blank_buckets)]++;
      }
    }
  }

  // Allocate final arrays
  wfl->num_blank_entries = num_unique;
  wfl->blank_map_entries = malloc_or_die(num_unique * sizeof(WMPEntry));
  wfl->blank_bucket_starts =
      malloc_or_die((wfl->num_blank_buckets + 1) * sizeof(uint32_t));

  // Compute bucket starts
  uint32_t offset = 0;
  for (uint32_t b = 0; b < wfl->num_blank_buckets; b++) {
    wfl->blank_bucket_starts[b] = offset;
    offset += bucket_counts[b];
  }
  wfl->blank_bucket_starts[wfl->num_blank_buckets] = offset;

  // Reset for insertion
  memset(bucket_counts, 0, wfl->num_blank_buckets * sizeof(uint32_t));

  // Write entries directly
  if (num_pairs > 0) {
    BitRack current_rack = pairs[0].bit_rack;
    uint32_t current_bits = pairs[0].blank_letter_bit;

    for (uint32_t i = 1; i <= num_pairs; i++) {
      bool is_end = (i == num_pairs) ||
                    !bit_rack_equals(&pairs[i].bit_rack, &current_rack);
      if (!is_end) {
        current_bits |= pairs[i].blank_letter_bit;
      } else {
        uint32_t bucket_idx =
            bit_rack_get_bucket_index(&current_rack, wfl->num_blank_buckets);
        uint32_t entry_idx =
            wfl->blank_bucket_starts[bucket_idx] + bucket_counts[bucket_idx]++;
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

  if (a->sem) {
    thread_sem_release(a->sem);
  }
  return NULL;
}

// ============================================================================
// Phase 3: Build double-blank entries using scratch buffers
// ============================================================================

typedef struct {
  LengthScratchBuffers *scratch;
  WMPForLength *wfl;
  int length;
  ThreadSemaphore *sem;
} DoubleBlankBuildArg;

static void *build_double_blank_entries_direct(void *arg) {
  DoubleBlankBuildArg *a = (DoubleBlankBuildArg *)arg;
  LengthScratchBuffers *scratch = a->scratch;
  const BitRack *unique_racks = scratch->unique_racks;
  uint32_t num_racks = scratch->num_unique_racks;
  WMPForLength *wfl = a->wfl;
  int length = a->length;

  // Estimate max pairs
  size_t max_pairs = (size_t)num_racks * (size_t)length * (size_t)length / 2;
  size_t needed_size = sizeof(DoubleBlankPair) * max_pairs;

  // Reuse scratch buffers if large enough
  if (needed_size > scratch->scratch_size) {
    free(scratch->scratch1);
    free(scratch->scratch2);
    scratch->scratch1 = malloc_or_die(needed_size);
    scratch->scratch2 = malloc_or_die(needed_size);
    scratch->scratch_size = needed_size;
  }

  DoubleBlankPair *pairs = (DoubleBlankPair *)scratch->scratch1;
  DoubleBlankPair *temp = (DoubleBlankPair *)scratch->scratch2;
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

      uint32_t present2 =
          bit_rack_get_letter_mask(&rack) & ~((1U << ml1) - 1) & ~1U;
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

  // Sort
  radix_sort_double_blank_pairs(pairs, temp, num_pairs, scratch->radix_passes);

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
  if (wfl->num_double_blank_buckets < MIN_BUCKETS) {
    wfl->num_double_blank_buckets = MIN_BUCKETS;
  }

  // Resize bucket_counts if needed
  if (wfl->num_double_blank_buckets > scratch->bucket_counts_size) {
    free(scratch->bucket_counts);
    scratch->bucket_counts =
        calloc(wfl->num_double_blank_buckets, sizeof(uint32_t));
    scratch->bucket_counts_size = wfl->num_double_blank_buckets;
  } else {
    memset(scratch->bucket_counts, 0,
           wfl->num_double_blank_buckets * sizeof(uint32_t));
  }
  uint32_t *bucket_counts = scratch->bucket_counts;

  if (num_pairs > 0) {
    const BitRack *prev = &pairs[0].bit_rack;
    bucket_counts[bit_rack_get_bucket_index(prev,
                                            wfl->num_double_blank_buckets)]++;
    for (uint32_t i = 1; i < num_pairs; i++) {
      if (!bit_rack_equals(&pairs[i].bit_rack, prev)) {
        prev = &pairs[i].bit_rack;
        bucket_counts[bit_rack_get_bucket_index(
            prev, wfl->num_double_blank_buckets)]++;
      }
    }
  }

  // Allocate final arrays
  wfl->num_double_blank_entries = num_unique;
  wfl->double_blank_map_entries = malloc_or_die(num_unique * sizeof(WMPEntry));
  wfl->double_blank_bucket_starts =
      malloc_or_die((wfl->num_double_blank_buckets + 1) * sizeof(uint32_t));

  // Compute bucket starts
  uint32_t offset = 0;
  for (uint32_t b = 0; b < wfl->num_double_blank_buckets; b++) {
    wfl->double_blank_bucket_starts[b] = offset;
    offset += bucket_counts[b];
  }
  wfl->double_blank_bucket_starts[wfl->num_double_blank_buckets] = offset;

  // Reset for insertion
  memset(bucket_counts, 0, wfl->num_double_blank_buckets * sizeof(uint32_t));

  // Write entries directly
  if (num_pairs > 0) {
    uint32_t run_start = 0;
    for (uint32_t i = 1; i <= num_pairs; i++) {
      bool is_end =
          (i == num_pairs) ||
          !bit_rack_equals(&pairs[i].bit_rack, &pairs[i - 1].bit_rack);
      if (is_end) {
        BitRack rack = pairs[run_start].bit_rack;
        uint32_t first_blank_letters = 0;
        uint16_t last_pair = 0xFFFF;
        for (uint32_t j = run_start; j < i; j++) {
          if (pairs[j].packed_pair != last_pair) {
            first_blank_letters |= 1U << (pairs[j].packed_pair & 0xFF);
            last_pair = pairs[j].packed_pair;
          }
        }

        uint32_t bucket_idx =
            bit_rack_get_bucket_index(&rack, wfl->num_double_blank_buckets);
        uint32_t entry_idx = wfl->double_blank_bucket_starts[bucket_idx] +
                             bucket_counts[bucket_idx]++;
        WMPEntry *entry = &wfl->double_blank_map_entries[entry_idx];
        memset(entry->bucket_or_inline, 0, WMP_INLINE_VALUE_BYTES);
        entry->first_blank_letters = first_blank_letters;
        wmp_entry_write_bit_rack(entry, &rack);

        run_start = i;
      }
    }
  }

  if (a->sem) {
    thread_sem_release(a->sem);
  }
  return NULL;
}

// ============================================================================
// max_word_lookup_bytes calculation
// ============================================================================

static uint32_t wfl_get_word_count(const WMPForLength *wfl,
                                   const BitRack *bit_rack, int word_length) {
  uint32_t bucket_idx =
      bit_rack_get_bucket_index(bit_rack, wfl->num_word_buckets);
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

static uint32_t calculate_max_word_lookup_bytes(WMP *wmp) {
  uint32_t max_bytes = 0;
  for (int len = 2; len <= BOARD_DIM; len++) {
    WMPForLength *wfl = &wmp->wfls[len];
    for (uint32_t i = 0; i < wfl->num_double_blank_entries; i++) {
      const WMPEntry *entry = &wfl->double_blank_map_entries[i];
      BitRack rack = wmp_entry_read_bit_rack(entry);
      uint32_t first_blanks = entry->first_blank_letters;
      uint32_t total_words = 0;

      bit_rack_set_letter_count(&rack, BLANK_MACHINE_LETTER, 1);
      for (int ml1 = 1; ml1 < BIT_RACK_MAX_ALPHABET_SIZE; ml1++) {
        if (!(first_blanks & (1U << ml1))) {
          continue;
        }
        bit_rack_add_letter(&rack, (MachineLetter)ml1);

        uint32_t blank_bucket =
            bit_rack_get_bucket_index(&rack, wfl->num_blank_buckets);
        uint32_t bstart = wfl->blank_bucket_starts[blank_bucket];
        uint32_t bend = wfl->blank_bucket_starts[blank_bucket + 1];
        for (uint32_t bi = bstart; bi < bend; bi++) {
          BitRack blank_rack =
              wmp_entry_read_bit_rack(&wfl->blank_map_entries[bi]);
          if (bit_rack_equals(&blank_rack, &rack)) {
            uint32_t second_blanks = wfl->blank_map_entries[bi].blank_letters;
            for (int ml2 = ml1; ml2 < BIT_RACK_MAX_ALPHABET_SIZE; ml2++) {
              if (!(second_blanks & (1U << ml2))) {
                continue;
              }
              bit_rack_add_letter(&rack, (MachineLetter)ml2);
              bit_rack_set_letter_count(&rack, BLANK_MACHINE_LETTER, 0);
              total_words += wfl_get_word_count(wfl, &rack, len);
              bit_rack_set_letter_count(&rack, BLANK_MACHINE_LETTER, 1);
              bit_rack_take_letter(&rack, (MachineLetter)ml2);
            }
            break;
          }
        }
        bit_rack_take_letter(&rack, (MachineLetter)ml1);
      }

      uint32_t bytes = total_words * len;
      if (bytes > max_bytes) {
        max_bytes = bytes;
      }
    }
  }
  return max_bytes;
}

// ============================================================================
// Main entry point
//
// WMP construction uses a three-phase parallel approach:
//   Phase 1: Build word entries and extract unique racks (from words list)
//   Phase 2: Build single-blank entries (from unique racks)
//   Phase 3: Build double-blank entries (from unique racks)
//
// Each phase processes word lengths 2-15 in parallel. Lengths are sorted by
// workload (descending) before thread launch so heavy workloads start first,
// improving CPU utilization when there are more lengths than threads.
//
// A counting semaphore limits concurrent threads to respect the user's
// -threads N setting, ensuring politeness on shared machines.
// ============================================================================

// Sort lengths by work descending so heavy workloads (7-8 letter words) start
// first, and light workloads (2-3, 14-15 letter words) fill in gaps later.
static void sort_lengths_by_work(int *lengths, const uint32_t *work,
                                 int count) {
  // Simple insertion sort (count is at most 14)
  for (int i = 1; i < count; i++) {
    int key_len = lengths[i];
    uint32_t key_work = work[key_len];
    int j = i - 1;
    while (j >= 0 && work[lengths[j]] < key_work) {
      lengths[j + 1] = lengths[j];
      j--;
    }
    lengths[j + 1] = key_len;
  }
}

WMP *make_wmp_from_words(const DictionaryWordList *words,
                         const LetterDistribution *ld, int num_threads) {
  if (ld->distribution[BLANK_MACHINE_LETTER] > 2) {
    log_fatal("cannot create WMP with more than 2 blanks");
    return NULL;
  }

  // Use all cores if num_threads is 0
  if (num_threads <= 0) {
    num_threads = get_num_cores();
  }
  // Cap at BOARD_DIM - 1 (max number of word lengths: 2-15)
  if (num_threads > BOARD_DIM - 1) {
    num_threads = BOARD_DIM - 1;
  }

  const int total_words = dictionary_word_list_get_count(words);

  // Count words by length
  int num_words_by_length[BOARD_DIM + 1] = {0};
  for (int i = 0; i < total_words; i++) {
    const DictionaryWord *word = dictionary_word_list_get_word(words, i);
    num_words_by_length[dictionary_word_get_length(word)]++;
  }

  // Build sorted list of lengths with words (by work descending)
  int sorted_lengths[BOARD_DIM + 1];
  int num_active_lengths = 0;
  for (int len = 2; len <= BOARD_DIM; len++) {
    if (num_words_by_length[len] > 0) {
      sorted_lengths[num_active_lengths++] = len;
    }
  }
  // Sort by pair count descending (heaviest work first)
  uint32_t pair_counts[BOARD_DIM + 1] = {0};

  // Initialize scratch buffers for each length
  LengthScratchBuffers scratch[BOARD_DIM + 1] = {{0}};

  // Pre-allocate word pair buffers (these will be reused for
  // blank/double-blank)
  WordPair *pairs_by_length[BOARD_DIM + 1] = {NULL};
  WordPair *temp_by_length[BOARD_DIM + 1] = {NULL};

  for (int len = 2; len <= BOARD_DIM; len++) {
    if (num_words_by_length[len] > 0) {
      size_t size = sizeof(WordPair) * (size_t)num_words_by_length[len];
      pairs_by_length[len] = malloc_or_die(size);
      temp_by_length[len] = malloc_or_die(size);
      // Initialize scratch with these buffers
      scratch[len].scratch1 = pairs_by_length[len];
      scratch[len].scratch2 = temp_by_length[len];
      scratch[len].scratch_size = size;
      // Initial bucket_counts allocation
      scratch[len].bucket_counts = calloc(MIN_BUCKETS, sizeof(uint32_t));
      scratch[len].bucket_counts_size = MIN_BUCKETS;
      scratch[len].radix_passes = radix_passes_for_alphabet(ld_get_size(ld));
    }
  }

  // Generate pairs and count
  for (int i = 0; i < total_words; i++) {
    const DictionaryWord *word = dictionary_word_list_get_word(words, i);
    uint8_t len = dictionary_word_get_length(word);
    uint32_t idx = pair_counts[len]++;
    pairs_by_length[len][idx].bit_rack =
        bit_rack_create_from_dictionary_word(word);
    pairs_by_length[len][idx].word_index = (uint32_t)i;
  }

  // Now sort lengths by work (pair_counts)
  sort_lengths_by_work(sorted_lengths, pair_counts, num_active_lengths);

  WMP *wmp = malloc_or_die(sizeof(WMP));
  wmp->name = NULL;
  wmp->version = WMP_VERSION;
  wmp->board_dim = BOARD_DIM;

  // Initialize empty wfls for lengths with no words
  for (int len = 2; len <= BOARD_DIM; len++) {
    if (num_words_by_length[len] == 0) {
      WMPForLength *wfl = &wmp->wfls[len];
      // Word entries
      wfl->num_word_buckets = MIN_BUCKETS;
      wfl->num_word_entries = 0;
      wfl->num_uninlined_words = 0;
      wfl->word_bucket_starts =
          malloc_or_die((wfl->num_word_buckets + 1) * sizeof(uint32_t));
      memset(wfl->word_bucket_starts, 0,
             (wfl->num_word_buckets + 1) * sizeof(uint32_t));
      wfl->word_map_entries = malloc_or_die(sizeof(WMPEntry));
      wfl->word_letters = malloc_or_die(1);
      // Blank entries
      wfl->num_blank_buckets = MIN_BUCKETS;
      wfl->num_blank_entries = 0;
      wfl->blank_bucket_starts =
          malloc_or_die((wfl->num_blank_buckets + 1) * sizeof(uint32_t));
      memset(wfl->blank_bucket_starts, 0,
             (wfl->num_blank_buckets + 1) * sizeof(uint32_t));
      wfl->blank_map_entries = malloc_or_die(sizeof(WMPEntry));
      // Double-blank entries
      wfl->num_double_blank_buckets = MIN_BUCKETS;
      wfl->num_double_blank_entries = 0;
      wfl->double_blank_bucket_starts =
          malloc_or_die((wfl->num_double_blank_buckets + 1) * sizeof(uint32_t));
      memset(wfl->double_blank_bucket_starts, 0,
             (wfl->num_double_blank_buckets + 1) * sizeof(uint32_t));
      wfl->double_blank_map_entries = malloc_or_die(sizeof(WMPEntry));
      // Scratch buffers
      scratch[len].unique_racks = malloc_or_die(sizeof(BitRack));
      scratch[len].num_unique_racks = 0;
      scratch[len].scratch1 = malloc_or_die(sizeof(WordPair));
      scratch[len].scratch2 = malloc_or_die(sizeof(WordPair));
      scratch[len].scratch_size = sizeof(WordPair);
      scratch[len].bucket_counts = calloc(MIN_BUCKETS, sizeof(uint32_t));
      scratch[len].bucket_counts_size = MIN_BUCKETS;
      scratch[len].radix_passes = radix_passes_for_alphabet(ld_get_size(ld));
    }
  }

  // Initialize thread semaphore for limiting concurrency
  ThreadSemaphore sem;
  thread_sem_init(&sem, num_threads);

  printf("[WMP] Phase 1/4: Building word entries (%d words, %d threads)...\n",
         total_words, num_threads);
  fflush(stdout);

  // Phase 1: Build word entries in parallel and extract unique racks
  // Launch threads in work-descending order for better scheduling
  WordBuildArg word_args[BOARD_DIM + 1];
  cpthread_t word_threads[BOARD_DIM + 1];

  if (num_threads == 1) {
    // Single-threaded: run sequentially in work order
    for (int i = 0; i < num_active_lengths; i++) {
      int len = sorted_lengths[i];
      word_args[len].words = words;
      word_args[len].wfl = &wmp->wfls[len];
      word_args[len].length = len;
      word_args[len].pairs = pairs_by_length[len];
      word_args[len].temp = temp_by_length[len];
      word_args[len].pair_count = pair_counts[len];
      word_args[len].scratch = &scratch[len];
      word_args[len].sem = NULL;
      build_words_and_extract_racks(&word_args[len]);
    }
  } else {
    // Multi-threaded: use semaphore to limit concurrent threads
    for (int i = 0; i < num_active_lengths; i++) {
      int len = sorted_lengths[i];
      word_args[len].words = words;
      word_args[len].wfl = &wmp->wfls[len];
      word_args[len].length = len;
      word_args[len].pairs = pairs_by_length[len];
      word_args[len].temp = temp_by_length[len];
      word_args[len].pair_count = pair_counts[len];
      word_args[len].scratch = &scratch[len];
      word_args[len].sem = &sem;
      thread_sem_acquire(&sem);
      cpthread_create(&word_threads[len], build_words_and_extract_racks,
                      &word_args[len]);
    }
    for (int i = 0; i < num_active_lengths; i++) {
      cpthread_join(word_threads[sorted_lengths[i]]);
    }
  }

  printf("[WMP] Phase 2/4: Building single-blank entries...\n");
  fflush(stdout);

  // Phase 2: Build blank entries in parallel (reusing scratch buffers)
  BlankBuildArg blank_args[BOARD_DIM + 1];
  cpthread_t blank_threads[BOARD_DIM + 1];

  if (num_threads == 1) {
    for (int i = 0; i < num_active_lengths; i++) {
      int len = sorted_lengths[i];
      blank_args[len].scratch = &scratch[len];
      blank_args[len].wfl = &wmp->wfls[len];
      blank_args[len].length = len;
      blank_args[len].sem = NULL;
      build_blank_entries_direct(&blank_args[len]);
    }
  } else {
    for (int i = 0; i < num_active_lengths; i++) {
      int len = sorted_lengths[i];
      blank_args[len].scratch = &scratch[len];
      blank_args[len].wfl = &wmp->wfls[len];
      blank_args[len].length = len;
      blank_args[len].sem = &sem;
      thread_sem_acquire(&sem);
      cpthread_create(&blank_threads[len], build_blank_entries_direct,
                      &blank_args[len]);
    }
    for (int i = 0; i < num_active_lengths; i++) {
      cpthread_join(blank_threads[sorted_lengths[i]]);
    }
  }

  printf("[WMP] Phase 3/4: Building double-blank entries...\n");
  fflush(stdout);

  // Phase 3: Build double-blank entries in parallel (reusing scratch buffers)
  DoubleBlankBuildArg dbl_args[BOARD_DIM + 1];
  cpthread_t dbl_threads[BOARD_DIM + 1];

  if (num_threads == 1) {
    for (int i = 0; i < num_active_lengths; i++) {
      int len = sorted_lengths[i];
      dbl_args[len].scratch = &scratch[len];
      dbl_args[len].wfl = &wmp->wfls[len];
      dbl_args[len].length = len;
      dbl_args[len].sem = NULL;
      build_double_blank_entries_direct(&dbl_args[len]);
    }
  } else {
    for (int i = 0; i < num_active_lengths; i++) {
      int len = sorted_lengths[i];
      dbl_args[len].scratch = &scratch[len];
      dbl_args[len].wfl = &wmp->wfls[len];
      dbl_args[len].length = len;
      dbl_args[len].sem = &sem;
      thread_sem_acquire(&sem);
      cpthread_create(&dbl_threads[len], build_double_blank_entries_direct,
                      &dbl_args[len]);
    }
    for (int i = 0; i < num_active_lengths; i++) {
      cpthread_join(dbl_threads[sorted_lengths[i]]);
    }
  }

  // Free scratch buffers
  for (int len = 2; len <= BOARD_DIM; len++) {
    free(scratch[len].scratch1);
    free(scratch[len].scratch2);
    free(scratch[len].bucket_counts);
    free(scratch[len].unique_racks);
  }

  printf("[WMP] Phase 4/4: Calculating lookup table sizes...\n");
  fflush(stdout);

  // Calculate max_word_lookup_bytes
  wmp->max_word_lookup_bytes = calculate_max_word_lookup_bytes(wmp);

  printf("[WMP] Build complete.\n");
  fflush(stdout);

  return wmp;
}

WMP *make_wmp_from_kwg(const KWG *kwg, const LetterDistribution *ld,
                       int num_threads) {
  DictionaryWordList *words = dictionary_word_list_create();
  kwg_write_words(kwg, kwg_get_dawg_root_node_index(kwg), words, NULL);
  WMP *wmp = make_wmp_from_words(words, ld, num_threads);
  dictionary_word_list_destroy(words);
  return wmp;
}
