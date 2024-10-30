#include <limits.h>
#include <stdint.h>

#include "../def/wmp_defs.h"

#include "../ent/bit_rack.h"
#include "../ent/dictionary_word.h"
#include "../ent/rack.h"
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
  return -1;
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

uint32_t
max_blank_pair_result_for_bucket(const MutableDoubleBlankMapBucket *bucket) {
  uint32_t max_size = 0;
  for (uint32_t entry_idx = 0; entry_idx < bucket->num_entries; entry_idx++) {
    const MutableDoubleBlankMapEntry *entry = &bucket->entries[entry_idx];
    const uint32_t size =
        2 * dictionary_word_list_get_count(entry->letter_pairs);
    if (size > max_size) {
      max_size = size;
    }
  }
  return max_size;
}

uint32_t max_blank_pair_result_for_length(
    const MutableDoubleBlanksForSameLengthMap *map) {
  uint32_t max_size = 0;
  for (uint32_t bucket_idx = 0; bucket_idx < map->num_double_blank_buckets;
       bucket_idx++) {
    const MutableDoubleBlankMapBucket *bucket =
        &map->double_blank_buckets[bucket_idx];
    const uint32_t size = max_blank_pair_result_for_bucket(bucket);
    if (size > max_size) {
      max_size = size;
    }
  }
  return max_size;
}

uint32_t
max_blank_pair_result_size(const MutableDoubleBlankMap *double_blank_map) {
  uint32_t max_size = 0;
  for (int len = 2; len <= BOARD_DIM; len++) {
    const MutableDoubleBlanksForSameLengthMap *map =
        &double_blank_map->maps[len];
    const uint32_t size = max_blank_pair_result_for_length(map);
    if (size > max_size) {
      max_size = size;
    }
  }
  return max_size;
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

int mwfl_get_num_words(const MutableWordsOfSameLengthMap *mwfl) {
  int num_words = 0;
  for (uint32_t bucket_idx = 0; bucket_idx < mwfl->num_word_buckets;
       bucket_idx++) {
    const MutableWordMapBucket *bucket = &mwfl->word_buckets[bucket_idx];
    for (uint32_t entry_idx = 0; entry_idx < bucket->num_entries; entry_idx++) {
      const MutableWordMapEntry *entry = &bucket->entries[entry_idx];
      num_words += dictionary_word_list_get_count(entry->letters);
    }
  }
  return num_words;
}

void write_word_range(uint32_t word_start, uint32_t num_words,
                      uint8_t bytes[16]) {

  // 8 zero bytes at start of entry designating it is non-inline
  memset(bytes, 0, sizeof(uint64_t));

  // 4 bytes for word start (index into wfl->word_letters)
  uint32_t *p_word_start = (uint32_t *)bytes + 2;
  *p_word_start = word_start;

  // 4 bytes for number of words in anagram set
  uint32_t *p_num_words = (uint32_t *)bytes + 3;
  *p_num_words = num_words;
}

void write_letters(const MutableWordMapEntry *entry, uint32_t word_start,
                   int word_length, uint8_t *letters) {
  const int num_words = dictionary_word_list_get_count(entry->letters);
  for (int word_idx = 0; word_idx < num_words; word_idx++) {
    const DictionaryWord *word =
        dictionary_word_list_get_word(entry->letters, word_idx);
    const uint8_t *word_letters = dictionary_word_get_word(word);
    memory_copy(letters + word_start + word_idx * word_length, word_letters,
                word_length);
  }
}

uint32_t write_word_entries(const MutableWordMapBucket *bucket, int word_length,
                            WMPEntry *entries, uint8_t *letters,
                            uint32_t *word_start) {
  for (uint32_t entry_idx = 0; entry_idx < bucket->num_entries; entry_idx++) {
    const MutableWordMapEntry *entry = &bucket->entries[entry_idx];
    bit_rack_write_12_bytes(&entry->quotient, entries[entry_idx].quotient);
    const uint32_t num_words = dictionary_word_list_get_count(entry->letters);
    write_word_range(*word_start, num_words,
                     entries[entry_idx].bucket_or_inline);
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
  for (uint32_t bucket_idx = 0; bucket_idx <= mwfl->num_word_buckets;
       bucket_idx++) {
    const MutableWordMapBucket *bucket = &mwfl->word_buckets[bucket_idx];
    wfl->word_bucket_starts[bucket_idx] = entry_idx;
    entry_idx += bucket->num_entries;
  }
  wfl->word_bucket_starts[wfl->num_word_buckets] = entry_idx;

  const int num_words = mwfl_get_num_words(mwfl);
  wfl->word_letters = (uint8_t *)malloc_or_die(num_words * word_length);
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

void fill_words_of_same_length_map(
    const MutableWordsOfSameLengthMap *mwfl,
    const MutableBlanksForSameLengthMap *mbfl,
    const MutableDoubleBlanksForSameLengthMap *mdbfl, int length,
    WMPForLength *wfl) {
  fill_wfl_blankless(mwfl, length, wfl);
}

WMP *make_wmp_from_mutables(const MutableWordMap *word_map,
                            const MutableBlankMap *blank_map,
                            const MutableDoubleBlankMap *double_blank_map) {
  assert(word_map != NULL);
  assert(blank_map != NULL);
  assert(double_blank_map != NULL);
  WMP *wmp = malloc_or_die(sizeof(WMP));
  wmp->major_version = WORD_MAP_MAJOR_VERSION;
  wmp->minor_version = WORD_MAP_MINOR_VERSION;
  wmp->min_word_length = get_min_word_length(word_map);
  wmp->max_word_length = get_max_word_length(word_map);
  wmp->max_blank_pair_bytes = max_blank_pair_result_size(double_blank_map);
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

MutableWordMap *make_mwmp_from_words(const DictionaryWordList *words) {
  MutableWordMap *mwmp = malloc_or_die(sizeof(MutableWordMap));
  int num_words_by_length[BOARD_DIM] = {0};
  for (int word_idx = 0; word_idx < dictionary_word_list_get_count(words);
       word_idx++) {
    const DictionaryWord *word = dictionary_word_list_get_word(words, word_idx);
    num_words_by_length[dictionary_word_get_length(word)]++;
  }
  for (int len = 0; len <= BOARD_DIM; len++) {
    MutableWordsOfSameLengthMap *mwfl = &mwmp->maps[len];
    if (num_words_by_length[len] == 0) {
      mwfl->num_word_buckets = 0;
      mwfl->word_buckets = NULL;
      continue;
    }
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

WMP *make_wmp_from_words(const DictionaryWordList *words) {
  MutableWordMap *mutable_word_map = make_mwmp_from_words(words);
  MutableBlankMap *mutable_blank_map = NULL;
  MutableDoubleBlankMap *mutable_double_blank_map = NULL;
  WMP *wmp = make_wmp_from_mutables(mutable_word_map, mutable_blank_map,
                                    mutable_double_blank_map);
  return wmp;
}