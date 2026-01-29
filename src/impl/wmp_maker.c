#include "wmp_maker.h"

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
        // Handle NULL letters (ownership may have been transferred)
        if (entry->letters != NULL) {
          dictionary_word_list_destroy(entry->letters);
        }
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
        // Handle NULL letter_pairs (ownership may have been transferred)
        if (entry->letter_pairs != NULL) {
          dictionary_word_list_destroy(entry->letter_pairs);
        }
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

// Transfer entry directly using stored bit_rack instead of recomputing
void transfer_word_entry(MutableWordMapEntry *entry,
                         MutableWordsOfSameLengthMap *new_mwfl) {
  const uint32_t bucket_index =
      bit_rack_get_bucket_index(&entry->bit_rack, new_mwfl->num_word_buckets);
  MutableWordMapBucket *bucket = &new_mwfl->word_buckets[bucket_index];

  if (bucket->num_entries == bucket->capacity) {
    bucket->capacity *= 2;
    bucket->entries = realloc_or_die(
        bucket->entries, sizeof(MutableWordMapEntry) * bucket->capacity);
  }
  // Transfer ownership of the entry (including letters pointer)
  bucket->entries[bucket->num_entries] = *entry;
  bucket->num_entries++;
  // Null out source to prevent double-free during destroy
  entry->letters = NULL;
}

void transfer_entries_from_word_bucket(MutableWordMapBucket *bucket,
                                       MutableWordsOfSameLengthMap *new_mwfl) {
  for (uint32_t entry_idx = 0; entry_idx < bucket->num_entries; entry_idx++) {
    MutableWordMapEntry *entry = &bucket->entries[entry_idx];
    transfer_word_entry(entry, new_mwfl);
  }
}

void fill_resized_mwfl(MutableWordsOfSameLengthMap *mwfl,
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
    MutableWordMapBucket *bucket = &mwfl->word_buckets[bucket_idx];
    transfer_entries_from_word_bucket(bucket, new_mwfl);
  }
}

MutableWordMap *resize_mutable_word_map(MutableWordMap *mwmp) {
  MutableWordMap *resized_mwmp = malloc_or_die(sizeof(MutableWordMap));
  for (int len = 2; len <= BOARD_DIM; len++) {
    MutableWordsOfSameLengthMap *mwfl = &mwmp->maps[len];
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

// Transfer entry directly using stored bit_rack instead of reinserting pairs
void transfer_double_blank_entry(MutableDoubleBlankMapEntry *entry,
                                 MutableDoubleBlanksForSameLengthMap *new_mdbfl) {
  const uint32_t new_bucket_idx = bit_rack_get_bucket_index(
      &entry->bit_rack, new_mdbfl->num_double_blank_buckets);
  MutableDoubleBlankMapBucket *bucket =
      &new_mdbfl->double_blank_buckets[new_bucket_idx];

  if (bucket->num_entries == bucket->capacity) {
    bucket->capacity *= 2;
    bucket->entries = realloc_or_die(
        bucket->entries, sizeof(MutableDoubleBlankMapEntry) * bucket->capacity);
  }
  // Transfer ownership of the entry (including letter_pairs pointer)
  bucket->entries[bucket->num_entries] = *entry;
  bucket->num_entries++;
  // Null out source to prevent double-free during destroy
  entry->letter_pairs = NULL;
}

void transfer_entries_from_double_blank_bucket(
    MutableDoubleBlankMapBucket *bucket,
    MutableDoubleBlanksForSameLengthMap *new_mdbfl) {
  for (uint32_t entry_idx = 0; entry_idx < bucket->num_entries; entry_idx++) {
    MutableDoubleBlankMapEntry *entry = &bucket->entries[entry_idx];
    transfer_double_blank_entry(entry, new_mdbfl);
  }
}

void fill_resized_mdbfl(MutableDoubleBlanksForSameLengthMap *mdbfl,
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
    MutableDoubleBlankMapBucket *bucket =
        &mdbfl->double_blank_buckets[bucket_idx];
    transfer_entries_from_double_blank_bucket(bucket, new_mdbfl);
  }
}

MutableDoubleBlankMap *
resize_mutable_double_blank_map(MutableDoubleBlankMap *mdbmp) {
  MutableDoubleBlankMap *resized_mdbmp =
      malloc_or_die(sizeof(MutableDoubleBlankMap));
  for (int len = 2; len <= BOARD_DIM; len++) {
    MutableDoubleBlanksForSameLengthMap *mdbfl = &mdbmp->maps[len];
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
  MutableWordMap *mwmp = make_mwmp_from_words(words);

  // Build blank map and double-blank map sequentially
  // (Parallel execution crashes with optimized builds due to malloc/thread
  // interaction issues)
  MutableBlankMap *mbmp = make_mutable_blank_map_from_mwmp(mwmp);
  MutableDoubleBlankMap *mdbmp = make_mutable_double_blank_map_from_mwmp(mwmp);

  MutableWordMap *resized_mwmp = resize_mutable_word_map(mwmp);
  mutable_word_map_destroy(mwmp);

  MutableBlankMap *resized_mbmp = resize_mutable_blank_map(mbmp);
  mutable_blank_map_destroy(mbmp);

  MutableDoubleBlankMap *resized_mdbmp = resize_mutable_double_blank_map(mdbmp);
  mutable_double_blank_map_destroy(mdbmp);

  WMP *wmp = make_wmp_from_mutables(resized_mwmp, resized_mbmp, resized_mdbmp);
  mutable_word_map_destroy(resized_mwmp);
  mutable_blank_map_destroy(resized_mbmp);
  mutable_double_blank_map_destroy(resized_mdbmp);
  return wmp;
}