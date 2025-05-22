#include <assert.h>
#include <limits.h>
#include <stdint.h>

#include "../def/wmp_defs.h"

#include "../ent/bit_rack.h"
#include "../ent/dictionary_word.h"
#include "../ent/wmp.h"

#include "wmp_maker.h"

typedef struct MutableWordsMapEntry {
  BitRack quotient;
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
  BitRack quotient;
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

typedef struct MutableDoubleBlankMapEntry {
  BitRack quotient;

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

uint32_t next_prime(uint32_t n) {
  if (n <= 2) {
    return 2;
  }
  if (n % 2 == 0) {
    n++;
  }
  for (int i = n;; i += 2) {
    bool is_prime = true;
    for (int j = 3; j * j <= i; j += 2) {
      if (i % j == 0) {
        is_prime = false;
        break;
      }
    }
    if (is_prime) {
      return i;
    }
  }
#if defined(__has_builtin) && __has_builtin(__builtin_unreachable)
  __builtin_unreachable();
#else
  return 0;
#endif
}

int get_min_word_length(const MutableWordMap *word_map) {
  for (int i = 0; i <= BOARD_DIM; i++) {
    if (word_map->maps[i].num_word_buckets > 0) {
      return i;
    }
  }
  return INT_MAX;
}

int get_max_word_length(const MutableWordMap *word_map) {
  for (int i = BOARD_DIM; i >= 0; i--) {
    if (word_map->maps[i].num_word_buckets > 0) {
      return i;
    }
  }
  log_fatal("no words in word map");
  return -1;
}

uint32_t get_number_of_words(const MutableWordsOfSameLengthMap *map,
                             const BitRack *bit_rack) {
  BitRack quotient;
  uint32_t bucket_index;
  bit_rack_div_mod(bit_rack, map->num_word_buckets, &quotient, &bucket_index);
  const MutableWordMapBucket *bucket = &map->word_buckets[bucket_index];
  for (uint32_t i = 0; i < bucket->num_entries; i++) {
    const MutableWordMapEntry *entry = &bucket->entries[i];
    if (bit_rack_equals(&entry->quotient, &quotient)) {
      return dictionary_word_list_get_count(entry->letters);
    }
  }
  return 0;
}

BitRack
entry_get_bit_rack_without_blanks(const MutableDoubleBlankMapEntry *entry,
                                  uint32_t num_double_blank_buckets,
                                  uint32_t bucket_idx) {
  BitRack bit_rack = bit_rack_mul(&entry->quotient, num_double_blank_buckets);
  bit_rack_add_uint32(&bit_rack, bucket_idx);
  bit_rack_set_letter_count(&bit_rack, BLANK_MACHINE_LETTER, 0);
  return bit_rack;
}

uint32_t number_of_double_blank_words(const MutableWordsOfSameLengthMap *wfl,
                                      const MutableDoubleBlankMapEntry *entry,
                                      uint32_t num_double_blank_buckets,
                                      uint32_t bucket_idx) {
  uint32_t num_words = 0;
  const DictionaryWordList *pairs = entry->letter_pairs;
  const int num_pairs = dictionary_word_list_get_count(pairs);
  // Non-const because it is modified to add and take away the blank letters
  BitRack bit_rack = entry_get_bit_rack_without_blanks(
      entry, num_double_blank_buckets, bucket_idx);
  for (int pair_idx = 0; pair_idx < num_pairs; pair_idx++) {
    const DictionaryWord *pair = dictionary_word_list_get_word(pairs, pair_idx);
    const uint8_t ml1 = dictionary_word_get_word(pair)[0];
    const uint8_t ml2 = dictionary_word_get_word(pair)[1];
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
    const uint32_t words = number_of_double_blank_words(
        wfl, entry, dbfl->num_double_blank_buckets, bucket_idx);
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
                                 int word_length) {
  int num_uninlined_words = 0;
  const int max_inlined = max_inlined_words(word_length);
  for (uint32_t bucket_idx = 0; bucket_idx < mwfl->num_word_buckets;
       bucket_idx++) {
    const MutableWordMapBucket *bucket = &mwfl->word_buckets[bucket_idx];
    for (uint32_t entry_idx = 0; entry_idx < bucket->num_entries; entry_idx++) {
      const MutableWordMapEntry *entry = &bucket->entries[entry_idx];
      const int num_words = dictionary_word_list_get_count(entry->letters);
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
    const uint8_t *word_letters = dictionary_word_get_word(word);
    const int word_length = dictionary_word_get_length(word);
    memcpy(bytes + word_idx * word_length, word_letters, word_length);
  }
}

void write_uninlined_word_range(uint32_t word_start, uint32_t num_words,
                                WMPEntry *entry) {
  memset(&entry->bucket_or_inline, 0, sizeof(entry->bucket_or_inline));
  entry->word_start = word_start;
  entry->num_words = num_words;
}

void write_letters(const MutableWordMapEntry *entry, uint32_t word_start,
                   int word_length, uint8_t *letters) {
  const int num_words = dictionary_word_list_get_count(entry->letters);
  for (int word_idx = 0; word_idx < num_words; word_idx++) {
    const DictionaryWord *word =
        dictionary_word_list_get_word(entry->letters, word_idx);
    const uint8_t *word_letters = dictionary_word_get_word(word);
    memcpy(letters + word_start + word_idx * word_length, word_letters,
           word_length);
  }
}

uint32_t write_word_entries(const MutableWordMapBucket *bucket, int word_length,
                            WMPEntry *entries, uint8_t *letters,
                            uint32_t *word_start) {
  for (uint32_t entry_idx = 0; entry_idx < bucket->num_entries; entry_idx++) {
    const MutableWordMapEntry *entry = &bucket->entries[entry_idx];
    assert(bit_rack_fits_in_12_bytes(&entry->quotient));
    bit_rack_write_12_bytes(&entry->quotient, entries[entry_idx].quotient);
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
                        int word_length, WMPForLength *wfl) {
  wfl->num_word_buckets = mwfl->num_word_buckets;
  wfl->word_bucket_starts =
      (uint32_t *)malloc_or_die((wfl->num_word_buckets + 1) * sizeof(uint32_t));
  int entry_idx = 0;
  for (uint32_t bucket_idx = 0; bucket_idx < mwfl->num_word_buckets;
       bucket_idx++) {
    const MutableWordMapBucket *bucket = &mwfl->word_buckets[bucket_idx];
    wfl->word_bucket_starts[bucket_idx] = entry_idx;
    entry_idx += bucket->num_entries;
  }
  wfl->word_bucket_starts[wfl->num_word_buckets] = entry_idx;

  wfl->num_uninlined_words = mwfl_get_num_uninlined_words(mwfl, word_length);
  wfl->word_letters =
      (uint8_t *)malloc_or_die(wfl->num_uninlined_words * word_length);
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
  memset(wmp_entry->bucket_or_inline, 0, sizeof(WMPEntry));
  wmp_entry->blank_letters = entry->blank_letters;
  assert(bit_rack_fits_in_12_bytes(&entry->quotient));
  bit_rack_write_12_bytes(&entry->quotient, wmp_entry->quotient);
}

void fill_wfl_blanks(const MutableBlanksForSameLengthMap *mbfl,
                     WMPForLength *wfl) {
  wfl->num_blank_buckets = mbfl->num_blank_buckets;
  wfl->blank_bucket_starts = (uint32_t *)malloc_or_die(
      (wfl->num_blank_buckets + 1) * sizeof(uint32_t));
  int overall_entry_idx = 0;
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
    const uint8_t *pair_letters = dictionary_word_get_word(pair);
    first_blank_letters |= 1 << pair_letters[0];
  }
  return first_blank_letters;
}

void write_double_blank_wmp_entry(const MutableDoubleBlankMapEntry *entry,
                                  WMPEntry *wmp_entry) {
  memset(wmp_entry->bucket_or_inline, 0, sizeof(WMPEntry));
  const uint32_t first_blank_letters = mdbfl_get_first_blank_letters(entry);
  wmp_entry->first_blank_letters = first_blank_letters;
  assert(bit_rack_fits_in_12_bytes(&entry->quotient));
  bit_rack_write_12_bytes(&entry->quotient, wmp_entry->quotient);
}

void fill_wfl_double_blanks(const MutableDoubleBlanksForSameLengthMap *mdbl,
                            WMPForLength *wfl) {
  wfl->num_double_blank_buckets = mdbl->num_double_blank_buckets;
  wfl->double_blank_bucket_starts = (uint32_t *)malloc_or_die(
      (wfl->num_double_blank_buckets + 1) * sizeof(uint32_t));
  int overall_entry_idx = 0;
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
                                         BitRack quotient,
                                         const DictionaryWord *word) {
  for (uint32_t entry_idx = 0; entry_idx < bucket->num_entries; entry_idx++) {
    MutableWordMapEntry *entry = &bucket->entries[entry_idx];
    if (bit_rack_equals(&entry->quotient, &quotient)) {
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
  entry->quotient = quotient;
  // entry->letters is a DictionaryWordList. Once completed, it will become a
  // contiguous blob of letters like ANESTRIANTSIERNASTIER... in the final WMP
  entry->letters = dictionary_word_list_create_with_capacity(1);
  dictionary_word_list_add_word(entry->letters, dictionary_word_get_word(word),
                                dictionary_word_get_length(word));
  bucket->num_entries++;
}

uint32_t compute_min_num_buckets(const LetterDistribution *ld) {
  const BitRack bit_rack = largest_bit_rack_for_ld(ld);
  const uint64_t high64 = bit_rack_get_high_64(&bit_rack);
  const int num_quotient_bits = WMP_QUOTIENT_BYTES * 8;
  const uint64_t max_quotient_high = (1ULL << (num_quotient_bits - 64)) - 1;
  const int divisor = (high64 / max_quotient_high) + 1;
  BitRack actual_quotient;
  uint32_t remainder;
  bit_rack_div_mod(&bit_rack, divisor, &actual_quotient, &remainder);
  const uint64_t actual_quotient_high = bit_rack_get_high_64(&actual_quotient);
  assert(actual_quotient_high <= max_quotient_high);

  return next_prime(divisor);
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
    // This is ideally sized based on number of anagram sets rather than number
    // of words, but we won't know how many anagram sets there are until we
    // collect them by alphagram. We will resize all the hashtables after we
    // build these initial versions, and those will be the sizes used in the
    // final WMP written to disk.
    mwfl->num_word_buckets = next_prime(num_words_by_length[len]);
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
    BitRack quotient;
    uint32_t bucket_index;
    bit_rack_div_mod(&bit_rack, mwfl->num_word_buckets, &quotient,
                     &bucket_index);
    MutableWordMapBucket *bucket = &mwfl->word_buckets[bucket_index];
    mutable_word_map_bucket_insert_word(bucket, quotient, word);
  }
  return mwmp;
}

void mutable_blank_map_bucket_init(MutableBlankMapBucket *bucket) {
  bucket->num_entries = 0;
  bucket->capacity = 1;
  bucket->entries = malloc_or_die(sizeof(MutableBlankMapEntry));
}

// This is an annoyance. Because we only store the quotient in the entry rather
// than the full bit rack, we have to recompute it by reversing the div/mod.
BitRack entry_get_full_bit_rack(const MutableWordMapEntry *entry,
                                uint32_t num_word_buckets,
                                uint32_t word_bucket_idx) {
  BitRack full_bit_rack = bit_rack_mul(&entry->quotient, num_word_buckets);
  bit_rack_add_uint32(&full_bit_rack, word_bucket_idx);
  return full_bit_rack;
}

void set_blank_map_bit(MutableBlanksForSameLengthMap *mbfl,
                       const BitRack *bit_rack, uint8_t ml) {
  BitRack quotient;
  uint32_t bucket_index;
  bit_rack_div_mod(bit_rack, mbfl->num_blank_buckets, &quotient, &bucket_index);
  MutableBlankMapBucket *bucket = &mbfl->blank_buckets[bucket_index];
  for (uint32_t entry_idx = 0; entry_idx < bucket->num_entries; entry_idx++) {
    MutableBlankMapEntry *entry = &bucket->entries[entry_idx];
    if (bit_rack_equals(&entry->quotient, &quotient)) {
      entry->blank_letters |= 1 << ml;
      return;
    }
  }
  if (bucket->num_entries == bucket->capacity) {
    bucket->capacity *= 2;
    bucket->entries = realloc_or_die(
        bucket->entries, sizeof(MutableBlankMapEntry) * bucket->capacity);
  }
  MutableBlankMapEntry *entry = &bucket->entries[bucket->num_entries];
  entry->quotient = quotient;
  entry->blank_letters = (uint32_t)1 << ml;
  bucket->num_entries++;
}

void insert_blanks_from_word_entry(const MutableWordMapEntry *word_entry,
                                   uint32_t num_word_buckets,
                                   uint32_t word_bucket_idx,
                                   MutableBlanksForSameLengthMap *mbfl) {
  BitRack bit_rack =
      entry_get_full_bit_rack(word_entry, num_word_buckets, word_bucket_idx);
  for (uint8_t ml = 1; ml < BIT_RACK_MAX_ALPHABET_SIZE; ml++) {
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
                                    uint32_t num_word_buckets,
                                    uint32_t word_bucket_idx,
                                    MutableBlanksForSameLengthMap *mbfl) {
  for (uint32_t entry_idx = 0; entry_idx < mwfl_bucket->num_entries;
       entry_idx++) {
    const MutableWordMapEntry *word_entry = &mwfl_bucket->entries[entry_idx];
    insert_blanks_from_word_entry(word_entry, num_word_buckets, word_bucket_idx,
                                  mbfl);
  }
}

void fill_mbfl_from_mwfl(MutableBlanksForSameLengthMap *mbfl,
                         const MutableWordsOfSameLengthMap *mwfl, int length) {
  mbfl->num_blank_buckets = next_prime(mwfl->num_word_buckets * length);
  mbfl->blank_buckets =
      malloc_or_die(sizeof(MutableBlankMapBucket) * mbfl->num_blank_buckets);
  for (uint32_t bucket_idx = 0; bucket_idx < mbfl->num_blank_buckets;
       bucket_idx++) {
    mutable_blank_map_bucket_init(&mbfl->blank_buckets[bucket_idx]);
  }
  for (uint32_t bucket_idx = 0; bucket_idx < mwfl->num_word_buckets;
       bucket_idx++) {
    const MutableWordMapBucket *mwfl_bucket = &mwfl->word_buckets[bucket_idx];
    insert_blanks_from_word_bucket(mwfl_bucket, mwfl->num_word_buckets,
                                   bucket_idx, mbfl);
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
    MutableDoubleBlankMapBucket *bucket, BitRack quotient,
    const uint8_t *blanks_as_word) {
  for (uint32_t i = 0; i < bucket->num_entries; i++) {
    MutableDoubleBlankMapEntry *entry = &bucket->entries[i];
    if (bit_rack_equals(&entry->quotient, &quotient)) {
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
  entry->quotient = quotient;
  entry->letter_pairs = dictionary_word_list_create_with_capacity(1);
  dictionary_word_list_add_word(entry->letter_pairs, blanks_as_word, 2);
  bucket->num_entries++;
}

void insert_double_blanks_from_word_entry(
    const MutableWordMapEntry *word_entry, uint32_t num_word_buckets,
    uint32_t word_bucket_idx, MutableDoubleBlanksForSameLengthMap *mdbfl) {
  BitRack bit_rack =
      entry_get_full_bit_rack(word_entry, num_word_buckets, word_bucket_idx);
  uint8_t blanks_as_word[2];
  for (uint8_t ml1 = 1; ml1 < BIT_RACK_MAX_ALPHABET_SIZE; ml1++) {
    if (bit_rack_get_letter(&bit_rack, ml1) > 0) {
      bit_rack_take_letter(&bit_rack, ml1);
      bit_rack_add_letter(&bit_rack, BLANK_MACHINE_LETTER);
      blanks_as_word[0] = ml1;
      for (uint8_t ml2 = ml1; ml2 < BIT_RACK_MAX_ALPHABET_SIZE; ml2++) {
        if (bit_rack_get_letter(&bit_rack, ml2) > 0) {
          bit_rack_take_letter(&bit_rack, ml2);
          bit_rack_add_letter(&bit_rack, BLANK_MACHINE_LETTER);
          blanks_as_word[1] = ml2;
          BitRack quotient;
          uint32_t bucket_index;
          bit_rack_div_mod(&bit_rack, mdbfl->num_double_blank_buckets,
                           &quotient, &bucket_index);
          MutableDoubleBlankMapBucket *bucket =
              &mdbfl->double_blank_buckets[bucket_index];
          mutable_double_blank_map_bucket_insert_pair(bucket, quotient,
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
    const MutableWordMapBucket *mwfl_bucket, uint32_t num_word_buckets,
    uint32_t word_bucket_idx, MutableDoubleBlanksForSameLengthMap *mdbfl) {
  for (uint32_t entry_idx = 0; entry_idx < mwfl_bucket->num_entries;
       entry_idx++) {
    const MutableWordMapEntry *word_entry = &mwfl_bucket->entries[entry_idx];
    insert_double_blanks_from_word_entry(word_entry, num_word_buckets,
                                         word_bucket_idx, mdbfl);
  }
}

void fill_mdbfl_from_mwfl(MutableDoubleBlanksForSameLengthMap *mdbfl,
                          const MutableWordsOfSameLengthMap *mwfl, int length) {
  mdbfl->num_double_blank_buckets = next_prime(mwfl->num_word_buckets * length);
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
    insert_double_blanks_from_word_bucket(mwfl_bucket, mwfl->num_word_buckets,
                                          bucket_idx, mdbfl);
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

int mwfl_get_num_entries(const MutableWordsOfSameLengthMap *mwfl) {
  int num_sets = 0;
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
    BitRack quotient;
    uint32_t bucket_index;
    bit_rack_div_mod(&bit_rack, new_mwfl->num_word_buckets, &quotient,
                     &bucket_index);
    MutableWordMapBucket *bucket = &new_mwfl->word_buckets[bucket_index];
    mutable_word_map_bucket_insert_word(bucket, quotient, word);
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
                       const LetterDistribution *ld,
                       MutableWordsOfSameLengthMap *new_mwfl) {
  const int num_entries = mwfl_get_num_entries(mwfl);
  const uint32_t min_num_buckets = compute_min_num_buckets(ld);
  new_mwfl->num_word_buckets = next_prime(num_entries);
  if (new_mwfl->num_word_buckets < min_num_buckets) {
    new_mwfl->num_word_buckets = min_num_buckets;
  }
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

MutableWordMap *resize_mutable_word_map(const MutableWordMap *mwmp,
                                        const LetterDistribution *ld) {
  MutableWordMap *resized_mwmp = malloc_or_die(sizeof(MutableWordMap));
  for (int len = 2; len <= BOARD_DIM; len++) {
    const MutableWordsOfSameLengthMap *mwfl = &mwmp->maps[len];
    MutableWordsOfSameLengthMap *new_mwfl = &resized_mwmp->maps[len];
    fill_resized_mwfl(mwfl, ld, new_mwfl);
  }
  return resized_mwmp;
}

void set_blank_map_bits(const BitRack *bit_rack, uint32_t blank_letters,
                        MutableBlanksForSameLengthMap *new_mbfl) {
  BitRack quotient;
  uint32_t bucket_idx;
  bit_rack_div_mod(bit_rack, new_mbfl->num_blank_buckets, &quotient,
                   &bucket_idx);
  MutableBlankMapBucket *bucket = &new_mbfl->blank_buckets[bucket_idx];
  // We're always adding a new k/v pair rather than modifying.
  if (bucket->num_entries == bucket->capacity) {
    bucket->capacity *= 2;
    bucket->entries = realloc_or_die(
        bucket->entries, sizeof(MutableBlankMapEntry) * bucket->capacity);
  }
  MutableBlankMapEntry *entry = &bucket->entries[bucket->num_entries];
  entry->quotient = quotient;
  entry->blank_letters = blank_letters;
  bucket->num_entries++;
}

void reinsert_blank_entry(const MutableBlankMapEntry *entry,
                          uint32_t old_num_buckets, uint32_t bucket_idx,
                          MutableBlanksForSameLengthMap *new_mbfl) {
  BitRack full_bit_rack = bit_rack_mul(&entry->quotient, old_num_buckets);
  bit_rack_add_uint32(&full_bit_rack, bucket_idx);
  set_blank_map_bits(&full_bit_rack, entry->blank_letters, new_mbfl);
}

void reinsert_entries_from_blank_bucket(
    const MutableBlankMapBucket *bucket, uint32_t old_num_buckets,
    uint32_t bucket_idx, MutableBlanksForSameLengthMap *new_mbfl) {
  for (uint32_t entry_idx = 0; entry_idx < bucket->num_entries; entry_idx++) {
    const MutableBlankMapEntry *entry = &bucket->entries[entry_idx];
    reinsert_blank_entry(entry, old_num_buckets, bucket_idx, new_mbfl);
  }
}

uint32_t mbfl_get_num_entries(const MutableBlanksForSameLengthMap *mbfl) {
  int num_entries = 0;
  for (uint32_t bucket_idx = 0; bucket_idx < mbfl->num_blank_buckets;
       bucket_idx++) {
    const MutableBlankMapBucket *bucket = &mbfl->blank_buckets[bucket_idx];
    num_entries += bucket->num_entries;
  }
  return num_entries;
}

void fill_resized_mbfl(const MutableBlanksForSameLengthMap *mbfl,
                       const LetterDistribution *ld,
                       MutableBlanksForSameLengthMap *new_mbfl) {
  const uint32_t num_entries = mbfl_get_num_entries(mbfl);
  const uint32_t min_num_buckets = compute_min_num_buckets(ld);
  new_mbfl->num_blank_buckets = next_prime(num_entries);
  if (new_mbfl->num_blank_buckets < min_num_buckets) {
    new_mbfl->num_blank_buckets = min_num_buckets;
  }
  new_mbfl->blank_buckets = malloc_or_die(sizeof(MutableBlankMapBucket) *
                                          new_mbfl->num_blank_buckets);
  for (uint32_t bucket_idx = 0; bucket_idx < new_mbfl->num_blank_buckets;
       bucket_idx++) {
    mutable_blank_map_bucket_init(&new_mbfl->blank_buckets[bucket_idx]);
  }
  for (uint32_t bucket_idx = 0; bucket_idx < mbfl->num_blank_buckets;
       bucket_idx++) {
    const MutableBlankMapBucket *bucket = &mbfl->blank_buckets[bucket_idx];
    reinsert_entries_from_blank_bucket(bucket, mbfl->num_blank_buckets,
                                       bucket_idx, new_mbfl);
  }
}

MutableBlankMap *resize_mutable_blank_map(const MutableBlankMap *mbmp,
                                          const LetterDistribution *ld) {
  MutableBlankMap *resized_mbmp = malloc_or_die(sizeof(MutableBlankMap));
  for (int len = 2; len <= BOARD_DIM; len++) {
    const MutableBlanksForSameLengthMap *mbfl = &mbmp->maps[len];
    MutableBlanksForSameLengthMap *new_mbfl = &resized_mbmp->maps[len];
    fill_resized_mbfl(mbfl, ld, new_mbfl);
  }
  return resized_mbmp;
}

uint32_t
mdbfl_get_num_entries(const MutableDoubleBlanksForSameLengthMap *mdbfl) {
  int num_entries = 0;
  for (uint32_t bucket_idx = 0; bucket_idx < mdbfl->num_double_blank_buckets;
       bucket_idx++) {
    const MutableDoubleBlankMapBucket *bucket =
        &mdbfl->double_blank_buckets[bucket_idx];
    num_entries += bucket->num_entries;
  }
  return num_entries;
}

void reinsert_double_blank_entry(
    const MutableDoubleBlankMapEntry *entry, uint32_t old_num_buckets,
    uint32_t bucket_idx, MutableDoubleBlanksForSameLengthMap *new_mdbfl) {
  BitRack full_bit_rack = bit_rack_mul(&entry->quotient, old_num_buckets);
  bit_rack_add_uint32(&full_bit_rack, bucket_idx);
  for (int pair_idx = 0;
       pair_idx < dictionary_word_list_get_count(entry->letter_pairs);
       pair_idx++) {
    const DictionaryWord *pair =
        dictionary_word_list_get_word(entry->letter_pairs, pair_idx);
    const uint8_t *pair_letters = dictionary_word_get_word(pair);
    BitRack quotient;
    uint32_t new_bucket_idx;
    bit_rack_div_mod(&full_bit_rack, new_mdbfl->num_double_blank_buckets,
                     &quotient, &new_bucket_idx);
    MutableDoubleBlankMapBucket *bucket =
        &new_mdbfl->double_blank_buckets[new_bucket_idx];
    mutable_double_blank_map_bucket_insert_pair(bucket, quotient, pair_letters);
  }
}

void reinsert_entries_from_double_blank_bucket(
    const MutableDoubleBlankMapBucket *bucket, uint32_t old_num_buckets,
    uint32_t bucket_idx, MutableDoubleBlanksForSameLengthMap *new_mdbfl) {
  for (uint32_t entry_idx = 0; entry_idx < bucket->num_entries; entry_idx++) {
    const MutableDoubleBlankMapEntry *entry = &bucket->entries[entry_idx];
    reinsert_double_blank_entry(entry, old_num_buckets, bucket_idx, new_mdbfl);
  }
}

void fill_resized_mdbfl(const MutableDoubleBlanksForSameLengthMap *mdbfl,
                        const LetterDistribution *ld,
                        MutableDoubleBlanksForSameLengthMap *new_mdbfl) {
  const uint32_t num_entries = mdbfl_get_num_entries(mdbfl);
  const uint32_t min_num_buckets = compute_min_num_buckets(ld);
  new_mdbfl->num_double_blank_buckets = next_prime(num_entries);
  if (new_mdbfl->num_double_blank_buckets < min_num_buckets) {
    new_mdbfl->num_double_blank_buckets = min_num_buckets;
  }
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
    reinsert_entries_from_double_blank_bucket(
        bucket, mdbfl->num_double_blank_buckets, bucket_idx, new_mdbfl);
  }
}

MutableDoubleBlankMap *
resize_mutable_double_blank_map(const MutableDoubleBlankMap *mdbmp,
                                const LetterDistribution *ld) {
  MutableDoubleBlankMap *resized_mdbmp =
      malloc_or_die(sizeof(MutableDoubleBlankMap));
  for (int len = 2; len <= BOARD_DIM; len++) {
    const MutableDoubleBlanksForSameLengthMap *mdbfl = &mdbmp->maps[len];
    MutableDoubleBlanksForSameLengthMap *new_mdbfl = &resized_mdbmp->maps[len];
    fill_resized_mdbfl(mdbfl, ld, new_mdbfl);
  }
  return resized_mdbmp;
}

WMP *make_wmp_from_words(const DictionaryWordList *words,
                         const LetterDistribution *ld) {
  if (ld->distribution[BLANK_MACHINE_LETTER] > 2) {
    log_fatal("cannot create WMP with more than 2 blanks");
    return NULL;
  }
  MutableWordMap *mwmp = make_mwmp_from_words(words);
  MutableBlankMap *mbmp = make_mutable_blank_map_from_mwmp(mwmp);
  MutableDoubleBlankMap *mdbmp = make_mutable_double_blank_map_from_mwmp(mwmp);

  MutableWordMap *resized_mwmp = resize_mutable_word_map(mwmp, ld);
  mutable_word_map_destroy(mwmp);

  MutableBlankMap *resized_mbmp = resize_mutable_blank_map(mbmp, ld);
  mutable_blank_map_destroy(mbmp);

  MutableDoubleBlankMap *resized_mdbmp =
      resize_mutable_double_blank_map(mdbmp, ld);
  mutable_double_blank_map_destroy(mdbmp);

  WMP *wmp = make_wmp_from_mutables(resized_mwmp, resized_mbmp, resized_mdbmp);
  mutable_word_map_destroy(resized_mwmp);
  mutable_blank_map_destroy(resized_mbmp);
  mutable_double_blank_map_destroy(resized_mdbmp);
  return wmp;
}